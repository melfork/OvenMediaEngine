//==============================================================================
//
//  TranscodeStream
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include <unistd.h>

#include "transcode_stream.h"
#include "transcode_application.h"
#include "utilities.h"

#include <config/config_manager.h>

#define OV_LOG_TAG "TranscodeStream"

TranscodeStream::TranscodeStream(std::shared_ptr<StreamInfo> stream_info, TranscodeApplication *parent)
{
	logtd("Created Transcode stream. name(%s)", stream_info->GetName().CStr());

	// 통계 정보 초기화
	_stats_decoded_frame_count = 0;

	// 부모 클래스
	_parent = parent;

	// 입력 스트림 정보	
	_stream_info_input = stream_info;

	// Prepare decoders
	for(auto &track : _stream_info_input->GetTracks())
	{
		CreateDecoder(track.second->GetId());
	}

	// TODO(dimiden): Read these values from config file
	auto app_info = parent->GetApplicationInfo();
	auto config = ConfigManager::Instance()->GetApplicationInfo(app_info->GetName());

	_transcode_context = std::make_shared<TranscodeContext>();

	_transcode_context->SetVideoCodecId(MediaCommonType::MediaCodecId::Vp8);
	_transcode_context->SetVideoBitrate(5000000);

	_transcode_context->SetVideoWidth(480);
	_transcode_context->SetVideoHeight(320);
	_transcode_context->SetFrameRate(30.00f);
	_transcode_context->SetGOP(30);
	_transcode_context->SetVideoTimeBase(1, 1000000);

	_transcode_context->SetAudioCodecId(MediaCommonType::MediaCodecId::Opus);
	_transcode_context->SetAudioBitrate(64000);
	_transcode_context->SetAudioSampleRate(48000);
	_transcode_context->GetAudioChannel().SetLayout(MediaCommonType::AudioChannel::Layout::LayoutStereo);
	_transcode_context->SetAudioSampleFormat(MediaCommonType::AudioSample::Format::S16);
	_transcode_context->SetAudioTimeBase(1, 1000000);

	///////////////////////////////////////////////////////
	// 트랜스코딩된 스트림을 생성함
	///////////////////////////////////////////////////////
	// TODO(soulk): 트랜스코딩 프로파일이 여러 개 되는 경우 처리 (TranscodeSteam 모듈에서 Output Stream Info를 여러개 생성해서 처리할지 구조적으로 검토를 해봐야함.)
	_stream_info_output = std::make_shared<StreamInfo>();

	// TODO(dimiden): Read stream name rule from config file
	_stream_info_output->SetName(ov::String::FormatString("%s_o", stream_info->GetName().CStr()));

	// Copy tracks
	for(auto &track : _stream_info_input->GetTracks())
	{
		// 기존 스트림의 미디어 트랙 정보
		auto &cur_track = track.second;

		// 새로운 스트림의 트랙 정보
		auto new_track = std::make_shared<MediaTrack>();

		new_track->SetId(cur_track->GetId());
		new_track->SetMediaType(cur_track->GetMediaType());

		switch(cur_track->GetMediaType())
		{
			case MediaCommonType::MediaType::Video:
				new_track->SetCodecId(_transcode_context->GetVideoCodecId());
				new_track->SetWidth(_transcode_context->GetVideoWidth());
				new_track->SetHeight(_transcode_context->GetVideoHeight());
				new_track->SetFrameRate(_transcode_context->GetFrameRate());
				new_track->SetTimeBase(_transcode_context->GetVideoTimeBase().GetNum(), _transcode_context->GetVideoTimeBase().GetDen());
				break;

			case MediaCommonType::MediaType::Audio:
				new_track->SetCodecId(_transcode_context->GetAudioCodecId());
				new_track->SetSampleRate(_transcode_context->GetAudioSampleRate());
				new_track->GetSample().SetFormat(_transcode_context->GetAudioSample().GetFormat());
				new_track->GetChannel().SetLayout(_transcode_context->GetAudioChannel().GetLayout());
				new_track->SetTimeBase(_transcode_context->GetAudioTimeBase().GetNum(), _transcode_context->GetAudioTimeBase().GetDen());
				break;

			default:
				// Media type is not supported.
				continue;
		}

		_stream_info_output->AddTrack(new_track);

		// av_log_set_level(AV_LOG_DEBUG);
		CreateEncoder(new_track, _transcode_context);
	}

	// 패킷 저리 스레드 생성
	try
	{
		_kill_flag = false;

		_thread_decode = std::thread(&TranscodeStream::DecodeTask, this);
		_thread_filter = std::thread(&TranscodeStream::FilterTask, this);
		_thread_encode = std::thread(&TranscodeStream::EncodeTask, this);
	}
	catch(const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start transcode stream thread.");
	}

	logtd("Started transcode stream thread.");
}

TranscodeStream::~TranscodeStream()
{
	logtd("Destroyed Transcode Stream.  name(%s) id(%u)", _stream_info_input->GetName().CStr(), _stream_info_input->GetId());

	// 스레드가 종료가 안된경우를 확인해서 종료함
	if(_kill_flag != true)
	{
		Stop();
	}
}

void TranscodeStream::Stop()
{
	_kill_flag = true;

	logtd("wait for terminated trancode stream thread. kill_flag(%s)", _kill_flag ? "true" : "false");

	_queue.abort();
	_thread_decode.join();

	_queue_decoded.abort();

	_thread_filter.join();

	_queue_filterd.abort();

	_thread_encode.join();
}

std::shared_ptr<StreamInfo> TranscodeStream::GetStreamInfo()
{
	return _stream_info_input;
}

bool TranscodeStream::Push(std::unique_ptr<MediaPacket> packet)
{
	// logtd("Stage-1-1 : %f", (float)frame->GetPts());
	// 변경된 스트림을 큐에 넣음
	_queue.push(std::move(packet));

	return true;
}

// std::unique_ptr<MediaFrame> TranscodeStream::Pop()
// {
// 	return _queue.pop_unique();
// }

uint32_t TranscodeStream::GetBufferCount()
{
	return _queue.size();
}

void TranscodeStream::CreateDecoder(int32_t media_track_id)
{
	auto track = _stream_info_input->GetTrack(media_track_id);

	if(track == nullptr)
	{
		logte("media track allocation failed");

		return;
	}

	// create decoder for codec id
	_decoders[media_track_id] = std::move(TranscodeDecoder::CreateDecoder(track->GetCodecId()));
}

void TranscodeStream::CreateEncoder(std::shared_ptr<MediaTrack> media_track, std::shared_ptr<TranscodeContext> transcode_context)
{
	if(media_track == nullptr)
	{
		return;
	}

	// create encoder for codec id
	_encoders[media_track->GetId()] = std::move(TranscodeEncoder::CreateEncoder(media_track->GetCodecId(), transcode_context));
}

void TranscodeStream::ChangeOutputFormat(MediaFrame *buffer)
{
	if(buffer == nullptr)
	{
		logte("invalid media buffer");
		return;
	}

	int32_t track_id = buffer->GetTrackId();

	// 트랙 정보
	auto &track = _stream_info_input->GetTrack(track_id);
	if(track == nullptr)
	{
		logte("cannot find output media track. track_id(%d)", track_id);

		return;
	}

	if(track->GetMediaType() == MediaCommonType::MediaType::Video)
	{
		logtd("parsed form media buffer. width:%d, height:%d, format:%d", buffer->GetWidth(), buffer->GetHeight(), buffer->GetFormat());

		track->SetWidth(buffer->GetWidth());
		track->SetHeight(buffer->GetHeight());
		track->GetTimeBase().Set(1, 1000);

		_filters[track->GetId()] = std::make_unique<TranscodeFilter>(TranscodeFilterType::VideoRescaler, track, _transcode_context);
	}
	else if(track->GetMediaType() == MediaCommonType::MediaType::Audio)
	{
		logtd("parsed form media buffer. format(%d), bytes_per_sample(%d), nb_samples(%d), channels(%d), channel_layout(%d), sample_rate(%d)",
		      buffer->GetFormat(), buffer->GetBytesPerSample(), buffer->GetNbSamples(), buffer->GetChannels(), buffer->GetChannelLayout(), buffer->GetSampleRate()
		);

		track->SetSampleRate(buffer->GetSampleRate());
		track->GetSample().SetFormat(buffer->GetFormat<MediaCommonType::AudioSample::Format>());
		track->GetChannel().SetLayout(buffer->GetChannelLayout());
		track->GetTimeBase().Set(1, 1000);

		_filters[track->GetId()] = std::make_unique<TranscodeFilter>(TranscodeFilterType::AudioResampler, track, _transcode_context);
	}
}

TranscodeResult TranscodeStream::do_decode(int32_t track_id, std::unique_ptr<const MediaPacket> packet)
{
	////////////////////////////////////////////////////////
	// 1) 디코더에 전달함
	////////////////////////////////////////////////////////
	auto decoder_item = _decoders.find(track_id);

	if(decoder_item == _decoders.end())
	{
		return TranscodeResult::NoData;
	}

	TranscodeDecoder *decoder = decoder_item->second.get();
	decoder->SendBuffer(std::move(packet));

	while(true)
	{
		TranscodeResult result;
		auto ret_frame = decoder->RecvBuffer(&result);

		switch(result)
		{
			case TranscodeResult::FormatChanged:
				// output format change 이벤트가 발생하면...
				// filter 및 인코더를 여기서 다시 초기화 해줘야함.
				// 디코더에 의해서 포맷 정보가 새롭게 알게되거나, 변경됨을 나타내를 반환값

				// logte("changed output format");
				// 필터 컨테스트 생성
				// 인코더 커테스트 생성
				ret_frame->SetTrackId(track_id);

				ChangeOutputFormat(ret_frame.get());
				break;

			case TranscodeResult::DataReady:
				// 디코딩이 성공하면,
				ret_frame->SetTrackId(track_id);

				// 필터 단계로 전달
				// do_filter(track_id, std::move(ret_frame));

				_stats_decoded_frame_count++;

				if(_stats_decoded_frame_count % 300 == 0)
				{
					logtd("stats. rq(%d), dq(%d), fq(%d)", _queue.size(), _queue_decoded.size(), _queue_filterd.size());
				}

				_queue_decoded.push(std::move(ret_frame));
				break;

			default:
				// 에러, 또는 디코딩된 패킷이 없다면 종료
				return result;
		}
	}
}

TranscodeResult TranscodeStream::do_filter(int32_t track_id, std::unique_ptr<MediaFrame> frame)
{
	////////////////////////////////////////////////////////
	// 1) 디코더에 전달함
	////////////////////////////////////////////////////////
	auto filter = _filters.find(track_id);

	if(filter == _filters.end())
	{
		return TranscodeResult::NoData;
	}

	logd("TranscodeStream.Packet", "SendBuffer to do_filter()\n%s", ov::Dump(frame->GetBuffer(0), frame->GetBufferSize(0), 32).CStr());

	filter->second->SendBuffer(std::move(frame));

	while(true)
	{
		TranscodeResult result;
		auto ret_frame = filter->second->RecvBuffer(&result);

		// 에러, 또는 디코딩된 패킷이 없다면 종료
		switch(result)
		{
			case TranscodeResult::DataReady:
				ret_frame->SetTrackId(track_id);

				logd("Transcode.Packet", "Received from filter:\n%s", ov::Dump(ret_frame->GetBuffer(0), ret_frame->GetBufferSize(0), 32).CStr());

				// logtd("filtered frame. track_id(%d), pts(%.0f)", ret_frame->GetTrackId(), (float)ret_frame->GetPts());

				_queue_filterd.push(std::move(ret_frame));
				// do_encode(track_id, std::move(ret_frame));
				break;

			default:
				return result;
		}
	}

	return TranscodeResult::DataReady;
}

TranscodeResult TranscodeStream::do_encode(int32_t track_id, std::unique_ptr<const MediaFrame> frame)
{
	if(_encoders.find(track_id) == _encoders.end())
	{
		return TranscodeResult::NoData;
	}

	////////////////////////////////////////////////////////
	// 2) 인코더에 전달
	////////////////////////////////////////////////////////

	auto &encoder = _encoders[track_id];
	encoder->SendBuffer(std::move(frame));

	while(true)
	{
		TranscodeResult result;
		auto ret_packet = encoder->RecvBuffer(&result);

		if(static_cast<int>(result) < 0)
		{
			return result;
		}

		if(result == TranscodeResult::DataReady)
		{
			ret_packet->SetTrackId(track_id);

			// logtd("encoded packet. track_id(%d), pts(%.0f)", ret_packet->GetTrackId(), (float)ret_packet->GetPts());

			////////////////////////////////////////////////////////
			// 3) 미디어 라우더에 전달
			////////////////////////////////////////////////////////
			_parent->SendFrame(_stream_info_output, std::move(ret_packet));
		}
	}

	return TranscodeResult::DataReady;
}

// 디코딩 & 인코딩 스레드
void TranscodeStream::DecodeTask()
{
	// 스트림 생성 전송
	_parent->CreateStream(_stream_info_output);

	logtd("Started transcode stream decode thread");

	while(!_kill_flag)
	{
		// 큐에 있는 인코딩된 패킷을 읽어옴
		auto packet = _queue.pop_unique();
		if(packet == nullptr)
		{
			// logtw("invliad media buffer");
			continue;
		}

		// 패킷의 트랙 아이디를 조회
		int32_t track_id = packet->GetTrackId();

		do_decode(track_id, std::move(packet));
	}

	// 스트림 삭제 전송
	_parent->DeleteStream(_stream_info_output);

	logtd("Terminated transcode stream decode thread");
}

void TranscodeStream::FilterTask()
{
	logtd("Transcode filter thread is started");

	while(!_kill_flag)
	{
		// 큐에 있는 인코딩된 패킷을 읽어옴
		auto frame = _queue_decoded.pop_unique();
		if(frame == nullptr)
		{
			// logtw("invliad media buffer");
			continue;
		}

		// 패킷의 트랙 아이디를 조회
		int32_t track_id = frame->GetTrackId();

		// logtd("Stage-1-2 : %f", (float)frame->GetPts());
		do_filter(track_id, std::move(frame));
	}

	logtd("Transcode filter thread is terminated");
}

void TranscodeStream::EncodeTask()
{
	logtd("Started transcode stream encode thread");

	while(!_kill_flag)
	{
		// 큐에 있는 인코딩된 패킷을 읽어옴
		auto frame = _queue_filterd.pop_unique();
		if(frame == nullptr)
		{
			// logtw("invliad media buffer");
			continue;
		}

		// 패킷의 트랙 아이디를 조회
		int32_t track_id = frame->GetTrackId();

		// logtd("Stage-1-2 : %f", (float)frame->GetPts());
		do_encode(track_id, std::move(frame));
	}

	logtd("Terminated transcode stream encode thread");
}
