// Crux2 - stand-ins for what the console build leaves out.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Video capture (FFmpeg, which the other platforms load at run time) and the emulated network
// adapter's libpcap back end have no library on the console: the functions the rest of PCSX2
// calls are here, and say so. (The network adapter's "Sockets" back end, which needs no library,
// is built.)

#include "DEV9/pcap_io.h"
#include "GS/GSCapture.h"

#include "common/Threading.h"

//////////////////////////////////////////////////////////////////////////
// Video capture
//////////////////////////////////////////////////////////////////////////

bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
	return false;
}

bool GSCapture::DeliverVideoFrame(GSTexture* stex)
{
	return false;
}

void GSCapture::DeliverAudioPacket(const float* frames)
{
}

void GSCapture::EndCapture()
{
}

bool GSCapture::IsCapturing()
{
	return false;
}

bool GSCapture::IsCapturingVideo()
{
	return false;
}

bool GSCapture::IsCapturingAudio()
{
	return false;
}

TinyString GSCapture::GetElapsedTime()
{
	return {};
}

const Threading::ThreadHandle& GSCapture::GetEncoderThreadHandle()
{
	static const Threading::ThreadHandle s_no_thread;
	return s_no_thread;
}

GSVector2i GSCapture::GetSize()
{
	return GSVector2i(0, 0);
}

std::string GSCapture::GetNextCaptureFileName()
{
	return {};
}

void GSCapture::Flush()
{
}

void GSCapture::FlushAudioOnly()
{
}

GSCapture::CodecList GSCapture::GetVideoCodecList(const char* container)
{
	return {};
}

GSCapture::CodecList GSCapture::GetAudioCodecList(const char* container)
{
	return {};
}

GSCapture::FormatList GSCapture::GetVideoFormatList(const char* codec)
{
	return {};
}

//////////////////////////////////////////////////////////////////////////
// The network adapter's libpcap back end
//////////////////////////////////////////////////////////////////////////

PCAPAdapter::PCAPAdapter()
	: NetAdapter()
	, switched(false)
	, blocking(false)
{
}

PCAPAdapter::~PCAPAdapter() = default;

bool PCAPAdapter::blocks()
{
	return false;
}

bool PCAPAdapter::isInitialised()
{
	return false;
}

bool PCAPAdapter::recv(NetPacket* pkt)
{
	return false;
}

bool PCAPAdapter::send(NetPacket* pkt)
{
	return false;
}

void PCAPAdapter::reloadSettings()
{
}

std::vector<AdapterEntry> PCAPAdapter::GetAdapters()
{
	return {};
}

AdapterOptions PCAPAdapter::GetAdapterOptions()
{
	return AdapterOptions::None;
}
