#pragma once
#include <cstdint>
#include <string>
#include "Pad.h"

class c_svc_msg_voice_data
{
public:
	PAD(0x8);
	std::int32_t client;
	std::int32_t audible_mask;
	std::uint32_t xuid_low;
	std::uint32_t xuid_high;
	std::string* voice_data;
	bool proximity;
	bool caster;
	std::int32_t format;
	std::int32_t sequence_bytes;
	std::uint32_t section_number;
	std::uint32_t uncompressed_sample_offset;
};
