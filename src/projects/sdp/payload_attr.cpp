//==============================================================================
//
//  OvenMediaEngine
//
//  Created by getroot
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "payload_attr.h"

PayloadAttr::PayloadAttr()
{
	_id = 0;

	for(bool &flag : _rtcpfb_support_flag)
	{
		flag = false;
	}
}

PayloadAttr::~PayloadAttr()
{
}

void PayloadAttr::SetId(uint8_t id)
{
	_id = id;
}

uint8_t PayloadAttr::GetId()
{
	return _id;
}

bool PayloadAttr::SetRtpmap(const ov::String &codec, uint32_t rate, const ov::String &parameters)
{
	if(codec.UpperCaseString() == "VP8")
	{
		SetRtpmap(SupportCodec::Vp8, rate, parameters);
	}
	else if(codec.UpperCaseString() == "H264")
	{
		SetRtpmap(SupportCodec::H264, rate, parameters);
	}
	else if(codec.UpperCaseString() == "OPUS")
	{
		SetRtpmap(SupportCodec::Opus, rate, parameters);
	}
	else
	{
		return false;
	}

	return true;
}

// a=rtpmap:97 VP8/50000
// a=rtpmap:111 OPUS/48000/2
void PayloadAttr::SetRtpmap(const SupportCodec codec, uint32_t rate, const ov::String &parameters)
{
	_codec = codec;
	// codec에 따라 payload id를 발급한다.
	switch(_codec)
	{
		case SupportCodec::Vp8:
			if(_id == 0)
			{
				_id = 97;
			}
			_codec_str = "VP8";
			break;

		case SupportCodec::H264:
			if(_id == 0)
			{
				_id = 100;
			}
			_codec_str = "H264";
			break;

		case SupportCodec::Opus:
			if(_id == 0)
			{
				_id = 111;
			}
			_codec_str = "OPUS";
			break;

		default:
			_id = 0;
			return;
	}

	_rate = rate;
	_codec_param = parameters;
}

// a=rtcp-fb:96 nack pli
void PayloadAttr::EnableRtcpFb(const RtcpFbType &type, const bool on)
{
	_rtcpfb_support_flag[(int)type] = on;
}

const PayloadAttr::SupportCodec PayloadAttr::GetCodec()
{
	return _codec;
}

const ov::String PayloadAttr::GetCodecStr()
{
	return _codec_str;
}

const uint32_t PayloadAttr::GetCodecRate()
{
	return _rate;
}

const ov::String PayloadAttr::GetCodecParams()
{
	return _codec_param;
}

bool PayloadAttr::EnableRtcpFb(const ov::String &type, const bool on)
{
	ov::String type_name = type.UpperCaseString();

	if(type_name == "GOOG_REMB")
	{
		_rtcpfb_support_flag[(int)(RtcpFbType::GoogRemb)] = on;
	}
	else if(type_name == "TRANSPORT_CC")
	{
		_rtcpfb_support_flag[(int)(RtcpFbType::TransportCc)] = on;
	}
	else if(type_name == "CCM_FIR")
	{
		_rtcpfb_support_flag[(int)(RtcpFbType::CcmFir)] = on;
	}
	else if(type_name == "NACK")
	{
		_rtcpfb_support_flag[(int)(RtcpFbType::Nack)] = on;
	}
	else if(type_name == "NACK_PLI")
	{
		_rtcpfb_support_flag[(int)(RtcpFbType::NackPli)] = on;
	}
	else
	{
		return false;
	}

	return true;
}

bool PayloadAttr::IsRtcpFbEnabled(const PayloadAttr::RtcpFbType &type)
{
	return _rtcpfb_support_flag[(int)type];
}