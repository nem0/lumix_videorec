// Records the game view or the scene view to a video file with the hardware encoder in NVIDIA GPUs (NVENC).
//
// The engine runs with a fixed time step while recording (one step per captured frame), so the video is smooth
// even if the editor can't render in real time. If the encoder is too slow, the simulation is paused until it catches up.
// Frames are encoded directly from GPU memory, there is no read-back to the CPU.

#include <imgui/imgui.h>
#include <imgui/IconsFontAwesome5.h>
#include "core/allocator.h"
#include "core/array.h"
#include "core/log.h"
#include "core/os.h"
#include "core/stream.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/thread.h"
#include "editor/action.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/engine.h"
#include "engine/plugin.h"
#include "renderer/draw_stream.h"
#include "renderer/editor/game_view.h"
#include "renderer/editor/scene_view.h"
#include "renderer/gpu/gpu.h"
#include "renderer/pipeline.h"
#include "renderer/renderer.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <d3d12.h>
#include "nvenc/nvEncodeAPI.h"

namespace Lumix {

struct VideoEncoderSettings {
	enum Codec : u32 {
		AV1,	// royalty free, needs RTX 40 series or newer
		H264,
		HEVC
	};
	Codec codec = AV1;
	u32 width = 1920;			// size of the video, even
	u32 height = 1080;
	u32 source_width = 1920;	// size of the textures passed to `submit`, at least `width` x `height`
	u32 source_height = 1080;
	u32 fps = 60;
	u32 quality = 28;			// constant quality level, lower is better, 1-51
	u32 preset = 5;				// 1 (fastest) - 7 (best quality)
};

// number of frames that can be submitted and not yet collected
constexpr u32 VIDEO_ENCODER_MAX_IN_FLIGHT = 6;

// Minimal MP4 (ISO BMFF) muxer for a single video track: AV1, H.264 and HEVC.
//
// File layout: `ftyp`, `mdat` (64 bit size, patched when the recording ends), `moov` at the end. All samples are
// stored in one chunk, so the sample tables are tiny. No B-frames, no audio, constant frame rate.
//
// The caller writes the bytes of `writeHeader`, then the samples produced by `addPacket`, then patches the size of
// `mdat` (at `getMdatSizeOffset()`, 8 bytes, big endian, value `getMdatSize()`) and appends the bytes of `writeFooter`.
struct Mp4Writer {
	Mp4Writer(IAllocator& allocator)
		: m_allocator(allocator)
		, m_sample_sizes(allocator)
		, m_key_samples(allocator)
		, m_av1_config(allocator)
		, m_vps(allocator)
		, m_sps(allocator)
		, m_pps(allocator)
	{}

	void init(VideoEncoderSettings::Codec codec, u32 width, u32 height, u32 fps) {
		m_codec = codec;
		m_width = width;
		m_height = height;
		m_fps = fps;
	}

	// `ftyp` and the header of `mdat`
	void writeHeader(OutputMemoryStream& out) {
		const u32 start = (u32)out.size();
		beginBox(out, "ftyp");
		out.write("isom", 4);
		put32(out, 0x200);
		out.write("isom", 4);
		out.write("iso2", 4);
		out.write(m_codec == VideoEncoderSettings::AV1 ? "av01" : (m_codec == VideoEncoderSettings::H264 ? "avc1" : "hvc1"), 4);
		out.write("mp41", 4);
		endBox(out, start);

		// 64 bit size: 1, 'mdat', size
		put32(out, 1);
		out.write("mdat", 4);
		m_mdat_size_offset = out.size();
		put64(out, 16);
		m_first_sample_offset = out.size();
	}

	u64 getMdatSizeOffset() const { return m_mdat_size_offset; }
	u64 getMdatSize() const { return 16 + m_payload_size; }
	u32 getSampleCount() const { return m_sample_sizes.size(); }

	// Converts a packet of NVENC (AV1: low overhead OBUs, H.264 and HEVC: Annex B) to a sample and appends it to `out`.
	// Returns false if there is nothing to write.
	bool addPacket(const u8* data, u32 size, OutputMemoryStream& out) {
		const u64 start = out.size();
		bool key = false;
		if (m_codec == VideoEncoderSettings::AV1) addAv1(data, size, out, key);
		else addAnnexB(data, size, out, key);
		const u64 sample_size = out.size() - start;
		if (sample_size == 0) return false;
		m_sample_sizes.push((u32)sample_size);
		if (key) m_key_samples.push(m_sample_sizes.size());
		m_payload_size += sample_size;
		return true;
	}

	// `moov`
	bool writeFooter(OutputMemoryStream& out) {
		const u32 samples = m_sample_sizes.size();
		if (samples == 0) return false;
		const u32 timescale = m_fps * 1000;
		const u32 delta = 1000;
		const u32 duration = samples * delta;

		const u32 moov = beginBox(out, "moov");
			const u32 mvhd = beginFullBox(out, "mvhd", 0, 0);
				put32(out, 0);					// creation
				put32(out, 0);					// modification
				put32(out, timescale);
				put32(out, duration);
				put32(out, 0x00010000);			// rate
				put16(out, 0x0100);				// volume
				zero(out, 10);
				putMatrix(out);
				zero(out, 24);
				put32(out, 2);					// next track id
			endBox(out, mvhd);

			const u32 trak = beginBox(out, "trak");
				const u32 tkhd = beginFullBox(out, "tkhd", 0, 3);
					put32(out, 0);
					put32(out, 0);
					put32(out, 1);				// track id
					put32(out, 0);
					put32(out, duration);
					zero(out, 8);
					put16(out, 0);				// layer
					put16(out, 0);				// alternate group
					put16(out, 0);				// volume
					put16(out, 0);
					putMatrix(out);
					put32(out, m_width << 16);
					put32(out, m_height << 16);
				endBox(out, tkhd);

				const u32 mdia = beginBox(out, "mdia");
					const u32 mdhd = beginFullBox(out, "mdhd", 0, 0);
						put32(out, 0);
						put32(out, 0);
						put32(out, timescale);
						put32(out, duration);
						put16(out, 0x55C4);		// "und"
						put16(out, 0);
					endBox(out, mdhd);

					const u32 hdlr = beginFullBox(out, "hdlr", 0, 0);
						put32(out, 0);
						out.write("vide", 4);
						zero(out, 12);
						out.write("VideoHandler", 13);	// zero terminated
					endBox(out, hdlr);

					const u32 minf = beginBox(out, "minf");
						const u32 vmhd = beginFullBox(out, "vmhd", 0, 1);
							zero(out, 8);
						endBox(out, vmhd);

						const u32 dinf = beginBox(out, "dinf");
							const u32 dref = beginFullBox(out, "dref", 0, 0);
								put32(out, 1);
								const u32 url = beginFullBox(out, "url ", 0, 1);
								endBox(out, url);
							endBox(out, dref);
						endBox(out, dinf);

						const u32 stbl = beginBox(out, "stbl");
							writeSampleDescription(out);

							const u32 stts = beginFullBox(out, "stts", 0, 0);
								put32(out, 1);
								put32(out, samples);
								put32(out, delta);
							endBox(out, stts);

							if (!m_key_samples.empty() && m_key_samples.size() != samples) {
								const u32 stss = beginFullBox(out, "stss", 0, 0);
									put32(out, m_key_samples.size());
									for (u32 k : m_key_samples) put32(out, k);
								endBox(out, stss);
							}

							const u32 stsc = beginFullBox(out, "stsc", 0, 0);
								put32(out, 1);
								put32(out, 1);			// first chunk
								put32(out, samples);	// samples per chunk
								put32(out, 1);			// description index
							endBox(out, stsc);

							const u32 stsz = beginFullBox(out, "stsz", 0, 0);
								put32(out, 0);			// every sample has its own size
								put32(out, samples);
								for (u32 s : m_sample_sizes) put32(out, s);
							endBox(out, stsz);

							// one chunk, so one offset
							const u32 co64 = beginFullBox(out, "co64", 0, 0);
								put32(out, 1);
								put64(out, m_first_sample_offset);
							endBox(out, co64);
						endBox(out, stbl);
					endBox(out, minf);
				endBox(out, mdia);
			endBox(out, trak);
		endBox(out, moov);
		return true;
	}

private:
	// ---- boxes
	static void put16(OutputMemoryStream& out, u16 v) { const u8 b[] = { u8(v >> 8), u8(v) }; out.write(b, 2); }
	static void put32(OutputMemoryStream& out, u32 v) { const u8 b[] = { u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v) }; out.write(b, 4); }
	static void put64(OutputMemoryStream& out, u64 v) { put32(out, u32(v >> 32)); put32(out, u32(v)); }
	static void zero(OutputMemoryStream& out, u32 count) { for (u32 i = 0; i < count; ++i) { const u8 z = 0; out.write(&z, 1); } }

	static void putMatrix(OutputMemoryStream& out) {
		static const u32 MATRIX[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
		for (u32 v : MATRIX) put32(out, v);
	}

	// returns the offset of the box, to be passed to `endBox`
	static u32 beginBox(OutputMemoryStream& out, const char* type) {
		const u32 offset = (u32)out.size();
		put32(out, 0);
		out.write(type, 4);
		return offset;
	}

	static u32 beginFullBox(OutputMemoryStream& out, const char* type, u8 version, u32 flags) {
		const u32 offset = beginBox(out, type);
		put32(out, (u32(version) << 24) | (flags & 0xffFFff));
		return offset;
	}

	static void endBox(OutputMemoryStream& out, u32 offset) {
		const u32 size = u32(out.size()) - offset;
		u8* p = out.getMutableData() + offset;
		p[0] = u8(size >> 24);
		p[1] = u8(size >> 16);
		p[2] = u8(size >> 8);
		p[3] = u8(size);
	}

	void writeSampleDescription(OutputMemoryStream& out) {
		const u32 stsd = beginFullBox(out, "stsd", 0, 0);
		put32(out, 1);
		const char* type = m_codec == VideoEncoderSettings::AV1 ? "av01" : (m_codec == VideoEncoderSettings::H264 ? "avc1" : "hvc1");
		const u32 entry = beginBox(out, type);
			zero(out, 6);
			put16(out, 1);				// data reference index
			put16(out, 0);
			put16(out, 0);
			zero(out, 12);
			put16(out, (u16)m_width);
			put16(out, (u16)m_height);
			put32(out, 0x00480000);		// 72 dpi
			put32(out, 0x00480000);
			put32(out, 0);
			put16(out, 1);				// frame count
			zero(out, 32);				// compressor name
			put16(out, 0x0018);			// depth
			put16(out, 0xffFF);			// pre defined
			if (m_codec == VideoEncoderSettings::AV1) writeAv1C(out);
			else if (m_codec == VideoEncoderSettings::H264) writeAvcC(out);
			else writeHvcC(out);
		endBox(out, entry);
		endBox(out, stsd);
	}

	// ---- AV1
	static bool readLeb128(const u8* data, u32 size, u32& value, u32& length) {
		value = 0;
		for (u32 i = 0; i < 8 && i < size; ++i) {
			value |= u32(data[i] & 0x7f) << (i * 7);
			if (!(data[i] & 0x80)) {
				length = i + 1;
				return true;
			}
		}
		return false;
	}

	// removes temporal delimiters, which are not allowed in samples, keeps the rest as it is
	void addAv1(const u8* data, u32 size, OutputMemoryStream& out, bool& key) {
		u32 i = 0;
		while (i < size) {
			const u8 header = data[i];
			const u32 type = (header >> 3) & 15;
			const bool has_extension = (header >> 2) & 1;
			const bool has_size = (header >> 1) & 1;
			const u32 header_size = 1 + (has_extension ? 1 : 0);
			u32 payload_size = 0;
			u32 leb_size = 0;
			if (!has_size || i + header_size >= size || !readLeb128(data + i + header_size, size - i - header_size, payload_size, leb_size)) {
				// can't be split, the rest is one OBU
				out.write(data + i, size - i);
				break;
			}
			u32 total = header_size + leb_size + payload_size;
			if (i + total > size) total = size - i;
			if (type != 2) {	// temporal delimiter
				if (type == 1) {
					key = true;	// the encoder repeats the sequence header on every key frame
					if (m_av1_config.size() == 0) {
						m_av1_config.write(data + i, total);
						parseAv1SequenceHeader(data + i + header_size + leb_size, total - header_size - leb_size);
					}
				}
				out.write(data + i, total);
			}
			i += total;
		}
	}

	void parseAv1SequenceHeader(const u8* p, u32 size) {
		struct Bits {
			const u8* data;
			u32 size;
			u32 pos = 0;
			u32 get(u32 count) {
				u32 v = 0;
				for (u32 i = 0; i < count; ++i, ++pos) {
					const u32 byte = pos >> 3;
					const u32 bit = byte < size ? (data[byte] >> (7 - (pos & 7))) & 1 : 0;
					v = (v << 1) | bit;
				}
				return v;
			}
		} bits{p, size};
		m_av1_profile = bits.get(3);
		bits.get(1);	// still picture
		const bool reduced = bits.get(1);
		if (reduced) {
			m_av1_level = bits.get(5);
			m_av1_tier = 0;
			return;
		}
		const bool timing_info = bits.get(1);
		if (timing_info) return;	// the encoder does not write it, keep the defaults
		const bool initial_display_delay = bits.get(1);
		bits.get(5);				// operating points count - 1
		bits.get(12);				// idc of the first point
		m_av1_level = bits.get(5);
		m_av1_tier = m_av1_level > 7 ? bits.get(1) : 0;
		(void)initial_display_delay;
	}

	void writeAv1C(OutputMemoryStream& out) {
		const u32 box = beginBox(out, "av1C");
		const u8 b[4] = {
			0x81,																// marker, version
			u8((m_av1_profile << 5) | m_av1_level),
			u8((m_av1_tier << 7) | (0 << 6) | (0 << 5) | (0 << 4) | (1 << 3) | (1 << 2)),	// 8 bit 4:2:0
			0
		};
		out.write(b, 4);
		if (m_av1_config.size() > 0) out.write(m_av1_config.data(), m_av1_config.size());
		endBox(out, box);
	}

	// ---- H.264 / HEVC
	template <typename F> static void forEachNal(const u8* data, u32 size, F f) {
		u32 i = 0;
		u32 nal_start = 0;
		bool in_nal = false;
		while (i + 2 < size) {
			if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
				if (in_nal) {
					u32 end = i;
					while (end > nal_start && data[end - 1] == 0) --end;	// zero of a 4 byte start code
					if (end > nal_start) f(data + nal_start, end - nal_start);
				}
				i += 3;
				nal_start = i;
				in_nal = true;
			}
			else {
				++i;
			}
		}
		if (in_nal && nal_start < size) f(data + nal_start, size - nal_start);
	}

	void addAnnexB(const u8* data, u32 size, OutputMemoryStream& out, bool& key) {
		const bool h264 = m_codec == VideoEncoderSettings::H264;
		forEachNal(data, size, [&](const u8* nal, u32 nal_size) {
			const u32 type = h264 ? (nal[0] & 0x1f) : ((nal[0] >> 1) & 0x3f);
			if (h264) {
				if (type == 7) { if (m_sps.size() == 0) m_sps.write(nal, nal_size); return; }
				if (type == 8) { if (m_pps.size() == 0) m_pps.write(nal, nal_size); return; }
				if (type == 9) return;		// access unit delimiter
				if (type == 5) key = true;	// IDR
			}
			else {
				if (type == 32) { if (m_vps.size() == 0) m_vps.write(nal, nal_size); return; }
				if (type == 33) { if (m_sps.size() == 0) m_sps.write(nal, nal_size); return; }
				if (type == 34) { if (m_pps.size() == 0) m_pps.write(nal, nal_size); return; }
				if (type == 35) return;		// access unit delimiter
				if (type >= 16 && type <= 21) key = true;	// IRAP
			}
			put32(out, nal_size);
			out.write(nal, nal_size);
		});
	}

	void writeAvcC(OutputMemoryStream& out) {
		const u32 box = beginBox(out, "avcC");
		if (m_sps.size() >= 4 && m_pps.size() > 0) {
			const u8* sps = m_sps.data();
			const u8 head[] = { 1, sps[1], sps[2], sps[3], 0xff, 0xe1 };	// version, profile, compatibility, level, 4 byte lengths, 1 sps
			out.write(head, sizeof(head));
			put16(out, (u16)m_sps.size());
			out.write(sps, m_sps.size());
			const u8 pps_count = 1;
			out.write(&pps_count, 1);
			put16(out, (u16)m_pps.size());
			out.write(m_pps.data(), m_pps.size());
			const u8 profile = sps[1];
			if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
				const u8 ext[] = { 0xfc | 1, 0xf8 | 0, 0xf8 | 0, 0 };	// 4:2:0, 8 bit, no sps extensions
				out.write(ext, sizeof(ext));
			}
		}
		endBox(out, box);
	}

	void writeHvcC(OutputMemoryStream& out) {
		const u32 box = beginBox(out, "hvcC");
		if (m_sps.size() >= 15 && m_vps.size() > 0 && m_pps.size() > 0) {
			// profile, tier and level are copied from profile_tier_level() of the SPS (without emulation prevention bytes)
			u8 rbsp[16];
			u32 n = 0;
			const u8* sps = m_sps.data();
			for (u32 i = 2; i < m_sps.size() && n < sizeof(rbsp); ++i) {
				if (i >= 4 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue;
				rbsp[n++] = sps[i];
			}
			const u8 sub_layers_minus1 = (rbsp[0] >> 1) & 7;
			const u8 nested = rbsp[0] & 1;
			out.write("\x01", 1);					// version
			out.write(rbsp + 1, 1);					// profile space, tier, profile idc
			out.write(rbsp + 2, 4);					// compatibility flags
			out.write(rbsp + 6, 6);					// constraint flags
			out.write(rbsp + 12, 1);				// level
			put16(out, 0xf000);						// min spatial segmentation
			const u8 tail[] = {
				0xfc,								// parallelism type
				0xfc | 1,							// 4:2:0
				0xf8 | 0,							// luma 8 bit
				0xf8 | 0,							// chroma 8 bit
				0, 0,								// average frame rate
				u8(((sub_layers_minus1 + 1) << 3) | (nested << 2) | 3),	// constant frame rate 0, temporal layers, nesting, 4 byte lengths
				3									// arrays
			};
			out.write(tail, sizeof(tail));
			const OutputMemoryStream* units[] = { &m_vps, &m_sps, &m_pps };
			const u8 types[] = { 32, 33, 34 };
			for (u32 i = 0; i < 3; ++i) {
				const u8 header = 0x80 | types[i];	// array completeness, type
				out.write(&header, 1);
				put16(out, 1);						// number of units
				put16(out, (u16)units[i]->size());
				out.write(units[i]->data(), units[i]->size());
			}
		}
		endBox(out, box);
	}

	IAllocator& m_allocator;
	VideoEncoderSettings::Codec m_codec = VideoEncoderSettings::AV1;
	u32 m_width = 0;
	u32 m_height = 0;
	u32 m_fps = 60;
	u64 m_mdat_size_offset = 0;
	u64 m_first_sample_offset = 0;
	u64 m_payload_size = 0;
	Array<u32> m_sample_sizes;
	Array<u32> m_key_samples;		// 1 based indices
	OutputMemoryStream m_av1_config;	// sequence header OBU
	u32 m_av1_profile = 0;
	u32 m_av1_level = 8;
	u32 m_av1_tier = 0;
	OutputMemoryStream m_vps;
	OutputMemoryStream m_sps;
	OutputMemoryStream m_pps;
};

constexpr u32 SLOT_COUNT = VIDEO_ENCODER_MAX_IN_FLIGHT;

typedef NVENCSTATUS (NVENCAPI *CreateInstanceFn)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS (NVENCAPI *GetMaxSupportedVersionFn)(uint32_t*);

template <typename T> void releaseCom(T*& ptr) {
	if (ptr) ptr->Release();
	ptr = nullptr;
}

struct Slot {
	ID3D12Resource* input = nullptr;						// copy of the frame, input of NVENC
	ID3D12Resource* output = nullptr;						// readback buffer, output of NVENC
	u8* output_cpu = nullptr;
	ID3D12Fence* fence = nullptr;							// signaled by NVENC when the frame is encoded
	u64 fence_value = 0;
	NV_ENC_REGISTERED_PTR registered_input = nullptr;
	NV_ENC_REGISTERED_PTR registered_output = nullptr;
	NV_ENC_INPUT_PTR mapped_input = nullptr;
	NV_ENC_INPUT_PTR mapped_output = nullptr;
	NV_ENC_INPUT_RESOURCE_D3D12 input_resource = {};		// passed by pointer to NVENC, has to stay valid until the frame is collected
	NV_ENC_OUTPUT_RESOURCE_D3D12 output_resource = {};
};

// NVENC running on the D3D12 device of the renderer. Frames never leave the GPU:
//
//  render thread (inside a DrawStream lambda):
//    compute pass: RGBA8 frame -> BGRA8 texture (the only input format of the D3D12 interface is ARGB, B8G8R8A8)
//    -> copy to a texture of the ring -> map it as NVENC input, it waits for the fence of the current frame
//    -> nvEncEncodePicture, NVENC signals a fence of the slot when the bitstream is ready
//  worker thread:
//    wait for the fence of the oldest slot -> nvEncLockBitstream -> write to the file
//
// The ring textures are owned by the encoder, because the output of a pipeline is a transient render buffer
// that can be overwritten by the next frame before NVENC is done with it.
struct VideoEncoder {
	VideoEncoder(IAllocator& allocator, Renderer& renderer) : m_allocator(allocator), m_renderer(renderer), m_mux(allocator), m_sample(allocator) {}
	~VideoEncoder() { destroy(); }

	void setError(const char* msg) {
		if (m_error_capacity) copyString(Span<char>(m_error, m_error_capacity), msg);
		m_failed = true;
	}

	bool fail(NVENCSTATUS status, const char* what) {
		const char* details = m_session && m_fn.nvEncGetLastErrorString ? m_fn.nvEncGetLastErrorString(m_session) : nullptr;
		StaticString<512> msg(what, " failed (NVENC status ", (i32)status, ")");
		if (details && details[0]) msg.append(": ", details);
		setError(msg);
		return false;
	}

	bool writeFile(const void* data, u32 size) {
		if (!m_file.write(data, size)) {
			setError("Could not write to the output file");
			return false;
		}
		return true;
	}

	// ftyp and the header of mdat
	bool writeContainerHeader() {
		m_mux.init(m_settings.codec, m_settings.width, m_settings.height, m_settings.fps);
		OutputMemoryStream header(m_allocator);
		m_mux.writeHeader(header);
		return writeFile(header.data(), (u32)header.size());
	}

	bool createSlot(Slot& slot) {
		D3D12_HEAP_PROPERTIES default_heap = {};
		default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC tex = {};
		tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex.Width = m_settings.width;
		tex.Height = m_settings.height;
		tex.DepthOrArraySize = 1;
		tex.MipLevels = 1;
		tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		tex.SampleDesc.Count = 1;
		tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		tex.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (FAILED(m_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.input)))) {
			setError("Could not create a texture for the frames");
			return false;
		}

		D3D12_HEAP_PROPERTIES readback_heap = {};
		readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buf = {};
		buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buf.Width = m_output_size;
		buf.Height = 1;
		buf.DepthOrArraySize = 1;
		buf.MipLevels = 1;
		buf.Format = DXGI_FORMAT_UNKNOWN;
		buf.SampleDesc.Count = 1;
		buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(m_device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.output)))) {
			setError("Could not create the output buffer");
			return false;
		}
		D3D12_RANGE no_read = {};
		void* mapped = nullptr;
		if (FAILED(slot.output->Map(0, &no_read, &mapped))) {
			setError("Could not map the output buffer");
			return false;
		}
		slot.output_cpu = (u8*)mapped;
		if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)))) {
			setError("Could not create a fence");
			return false;
		}

		NV_ENC_REGISTER_RESOURCE reg = {};
		reg.version = NV_ENC_REGISTER_RESOURCE_VER;
		reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
		reg.width = m_settings.width;
		reg.height = m_settings.height;
		reg.pitch = 0;
		reg.subResourceIndex = 0;
		reg.resourceToRegister = slot.input;
		reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;	// B is the lowest byte, B8G8R8A8
		reg.bufferUsage = NV_ENC_INPUT_IMAGE;
		// the D3D12 interface fails with NV_ENC_ERR_INVALID_PARAM and no message unless the registration passes a fence point
		// with both bWait and bSignal set (and the CPU waits for it) and the format is ARGB (B8G8R8A8), same as NVIDIA's NvEncoderD3D12 sample
		NV_ENC_FENCE_POINT_D3D12 reg_fence = {};
		reg_fence.pFence = m_reg_fence;
		reg_fence.waitValue = m_reg_fence_value;
		reg_fence.bWait = 1;
		reg_fence.signalValue = ++m_reg_fence_value;
		reg_fence.bSignal = 1;
		reg.pInputFencePoint = &reg_fence;
		NVENCSTATUS st = m_fn.nvEncRegisterResource(m_session, &reg);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncRegisterResource (input)");
		if (m_reg_fence->GetCompletedValue() < reg_fence.signalValue) { m_reg_fence->SetEventOnCompletion(reg_fence.signalValue, m_event); WaitForSingleObject(m_event, 5000); }
		slot.registered_input = reg.registeredResource;

		reg = {};
		reg.version = NV_ENC_REGISTER_RESOURCE_VER;
		reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
		reg.width = m_output_size;
		reg.height = 1;
		reg.pitch = 0;
		reg.subResourceIndex = 0;
		reg.resourceToRegister = slot.output;
		reg.bufferFormat = NV_ENC_BUFFER_FORMAT_U8;
		reg.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;
		reg.pInputFencePoint = nullptr;
		st = m_fn.nvEncRegisterResource(m_session, &reg);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncRegisterResource (output)");
		slot.registered_output = reg.registeredResource;
		return true;
	}

	// Writes a message to `error` and returns false if the encoder can't be created, e.g. the renderer is not DX12, no NVIDIA GPU
	// or the GPU does not support the codec. The file at `path` has to include the extension. Has to be called on the main thread.
	bool init(const VideoEncoderSettings& settings, const char* path, char* error, u32 error_capacity) {
		m_settings = settings;
		m_error = error;
		m_error_capacity = error_capacity;
		if (error_capacity) error[0] = '\0';

		if (settings.width < 34 || settings.height < 18 || (settings.width & 1) || (settings.height & 1)) {
			setError("Resolution must be even and at least 34x18");
			return false;
		}
		if (settings.width > 65535 || settings.height > 65535 || settings.fps == 0
			|| settings.source_width < settings.width || settings.source_height < settings.height) {
			setError("Invalid resolution or frame rate");
			return false;
		}

		m_device = (ID3D12Device*)gpu::getDX12Device();
		if (!m_device) {
			setError("Video recording needs the DX12 renderer");
			return false;
		}
		m_device->AddRef();

		m_lib = LoadLibraryW(L"nvEncodeAPI64.dll");
		if (!m_lib) {
			setError("nvEncodeAPI64.dll not found, NVENC needs an NVIDIA GPU and driver");
			return false;
		}
		auto create_instance = (CreateInstanceFn)GetProcAddress(m_lib, "NvEncodeAPICreateInstance");
		auto get_max_version = (GetMaxSupportedVersionFn)GetProcAddress(m_lib, "NvEncodeAPIGetMaxSupportedVersion");
		if (!create_instance || !get_max_version) {
			setError("NVENC library is missing entry points");
			return false;
		}
		uint32_t max_version = 0;
		if (get_max_version(&max_version) != NV_ENC_SUCCESS || max_version < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
			setError("NVIDIA driver is too old for this NVENC API version, update the driver");
			return false;
		}
		m_fn = {};
		m_fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
		NVENCSTATUS st = create_instance(&m_fn);
		if (st != NV_ENC_SUCCESS) return fail(st, "NvEncodeAPICreateInstance");

		NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
		open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
		open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
		open.device = m_device;
		open.apiVersion = NVENCAPI_VERSION;
		st = m_fn.nvEncOpenEncodeSessionEx(&open, &m_session);
		if (st != NV_ENC_SUCCESS) {
			m_session = nullptr;
			return fail(st, "nvEncOpenEncodeSessionEx");
		}

		GUID codec_guid = NV_ENC_CODEC_AV1_GUID;
		GUID profile_guid = NV_ENC_AV1_PROFILE_MAIN_GUID;
		const char* codec_name = "AV1";
		switch (settings.codec) {
			case VideoEncoderSettings::AV1: break;
			case VideoEncoderSettings::H264:
				codec_guid = NV_ENC_CODEC_H264_GUID;
				profile_guid = NV_ENC_H264_PROFILE_HIGH_GUID;
				codec_name = "H.264";
				break;
			case VideoEncoderSettings::HEVC:
				codec_guid = NV_ENC_CODEC_HEVC_GUID;
				profile_guid = NV_ENC_HEVC_PROFILE_MAIN_GUID;
				codec_name = "HEVC";
				break;
		}

		uint32_t guid_count = 0;
		st = m_fn.nvEncGetEncodeGUIDCount(m_session, &guid_count);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncGetEncodeGUIDCount");
		GUID guids[32];
		if (guid_count > 32) guid_count = 32;
		uint32_t returned = 0;
		st = m_fn.nvEncGetEncodeGUIDs(m_session, guids, guid_count, &returned);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncGetEncodeGUIDs");
		bool supported = false;
		for (uint32_t i = 0; i < returned; ++i) {
			if (memcmp(&guids[i], &codec_guid, sizeof(GUID)) == 0) supported = true;
		}
		if (!supported) {
			StaticString<128> msg("This GPU can't encode ", codec_name);
			if (settings.codec == VideoEncoderSettings::AV1) msg.append(" (AV1 needs an RTX 40 series or newer GPU)");
			setError(msg);
			return false;
		}

		const GUID presets[] = {
			NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_PRESET_P4_GUID,
			NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID
		};
		const u32 preset = settings.preset < 1 ? 1 : (settings.preset > 7 ? 7 : settings.preset);
		const GUID preset_guid = presets[preset - 1];
		const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;

		NV_ENC_PRESET_CONFIG preset_config = {};
		preset_config.version = NV_ENC_PRESET_CONFIG_VER;
		preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
		st = m_fn.nvEncGetEncodePresetConfigEx(m_session, codec_guid, preset_guid, tuning, &preset_config);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncGetEncodePresetConfigEx");

		NV_ENC_CONFIG config = preset_config.presetCfg;
		config.version = NV_ENC_CONFIG_VER;
		config.profileGUID = profile_guid;
		const u32 gop = settings.fps * 2;
		config.gopLength = gop;
		config.frameIntervalP = 1;	// no B frames, output order is the same as input order
		config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
		config.rcParams.averageBitRate = 0;
		config.rcParams.maxBitRate = 0;
		config.rcParams.targetQuality = (uint8_t)(settings.quality < 1 ? 1 : (settings.quality > 51 ? 51 : settings.quality));
		config.rcParams.targetQualityLSB = 0;
		switch (settings.codec) {
			case VideoEncoderSettings::AV1: {
				NV_ENC_CONFIG_AV1& av1 = config.encodeCodecConfig.av1Config;
				av1.idrPeriod = gop;
				av1.repeatSeqHdr = 1;
				av1.outputAnnexBFormat = 0;
				av1.chromaFormatIDC = 1;
				#if NVENCAPI_MAJOR_VERSION >= 13
					av1.inputBitDepth = NV_ENC_BIT_DEPTH_8;
					av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
				#else
					av1.inputPixelBitDepthMinus8 = 0;
					av1.pixelBitDepthMinus8 = 0;
				#endif
				break;
			}
			case VideoEncoderSettings::H264: {
				NV_ENC_CONFIG_H264& h264 = config.encodeCodecConfig.h264Config;
				h264.idrPeriod = gop;
				h264.repeatSPSPPS = 1;
				h264.chromaFormatIDC = 1;
				break;
			}
			case VideoEncoderSettings::HEVC: {
				NV_ENC_CONFIG_HEVC& hevc = config.encodeCodecConfig.hevcConfig;
				hevc.idrPeriod = gop;
				hevc.repeatSPSPPS = 1;
				hevc.chromaFormatIDC = 1;
				#if NVENCAPI_MAJOR_VERSION >= 13
					hevc.inputBitDepth = NV_ENC_BIT_DEPTH_8;
					hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
				#else
					hevc.pixelBitDepthMinus8 = 0;
				#endif
				break;
			}
		}

		NV_ENC_INITIALIZE_PARAMS init = {};
		init.version = NV_ENC_INITIALIZE_PARAMS_VER;
		init.encodeGUID = codec_guid;
		init.presetGUID = preset_guid;
		init.encodeWidth = settings.width;
		init.encodeHeight = settings.height;
		init.darWidth = settings.width;
		init.darHeight = settings.height;
		init.frameRateNum = settings.fps;
		init.frameRateDen = 1;
		init.enablePTD = 1;
		init.encodeConfig = &config;
		init.maxEncodeWidth = settings.width;
		init.maxEncodeHeight = settings.height;
		init.tuningInfo = tuning;
		init.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;	// the D3D12 interface needs it here
		st = m_fn.nvEncInitializeEncoder(m_session, &init);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncInitializeEncoder");

		m_output_size = settings.width * settings.height * 4;
		if (m_output_size < 2 * 1024 * 1024) m_output_size = 2 * 1024 * 1024;

		// the compute pass converts the frames of the renderer (RGBA8) to the only format NVENC takes in the D3D12 interface
		m_swizzled = m_renderer.createTexture(settings.source_width
			, settings.source_height
			, 1
			, gpu::TextureFormat::BGRA8
			, gpu::TextureFlags::NO_MIPS | gpu::TextureFlags::COMPUTE_WRITE
			, Renderer::MemRef()
			, "videorec_bgra");
		if (!m_swizzled) {
			setError("Could not create a texture for the frames");
			return false;
		}
		for (gpu::BufferHandle& params : m_params) {
			Renderer::MemRef memory;
			memory.size = 256;
			params = m_renderer.createBuffer(memory, gpu::BufferFlags::MAPPABLE, "videorec_params");
		}
		static const char* SWIZZLE_SRC = R"#(
			struct Params {
				uint src;
				uint dst;
				uint width;
				uint height;
			};
			ConstantBuffer<Params> cb : register(b4);

			[numthreads(16, 16, 1)]
			void main(uint3 thread_id : SV_DispatchThreadID) {
				if (thread_id.x >= cb.width || thread_id.y >= cb.height) return;
				// the output of a pipeline is an sRGB texture, the shader sees linear values and the BGRA8 target is not sRGB
				float4 v = bindless_textures[cb.src][thread_id.xy];
				float3 lo = v.rgb * 12.92;
				float3 hi = 1.055 * pow(max(v.rgb, 0.0031308), 1.0 / 2.4) - 0.055;
				v.rgb = lerp(hi, lo, step(v.rgb, 0.0031308));
				bindless_rw_textures[cb.dst][thread_id.xy] = v;
			}
		)#";
		m_program = gpu::allocProgramHandle();
		m_renderer.getDrawStream().createProgram(m_program, gpu::StateFlags::NONE, gpu::VertexDecl(gpu::PrimitiveType::NONE), SWIZZLE_SRC, gpu::ShaderType::COMPUTE, nullptr, 0, "videorec_bgra");

		m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_reg_fence));
		for (Slot& slot : m_slots) {
			if (!createSlot(slot)) return false;
		}

		if (!m_file.open(path)) {
			setError("Could not create the output file");
			return false;
		}
		m_file_open = true;
		if (!writeContainerHeader()) return false;
		return true;
	}

	// Render thread, copies the frame and submits it to NVENC, the result is collected by `collect`.
	bool submit(gpu::TextureHandle texture) {
		if (m_failed || !m_session) return false;
		u32 index;
		{
			MutexGuard guard(m_mutex);
			if (m_submitted - m_collected >= SLOT_COUNT) {
				setError("The encoder can't keep up");
				return false;
			}
			index = u32(m_submitted % SLOT_COUNT);
		}
		Slot& slot = m_slots[index];

		void* frame_fence = nullptr;
		u64 frame_fence_value = 0;
		if (!gpu::getDX12FrameFence(&frame_fence, &frame_fence_value)) {
			setError("Could not get the fence of the frame");
			return false;
		}

		// RGBA8 -> BGRA8 (the typed UAV store swaps the channels), the engine tracks the states of both textures
		struct { u32 src; u32 dst; u32 width; u32 height; } params = {
			gpu::getBindlessHandle(texture).value,
			gpu::getRWBindlessHandle(m_swizzled).value,
			m_settings.source_width,
			m_settings.source_height
		};
		void* params_ptr = gpu::map(m_params[index], sizeof(params));
		memcpy(params_ptr, &params, sizeof(params));
		gpu::unmap(m_params[index]);
		gpu::barrier(texture, gpu::BarrierType::READ);
		gpu::barrier(m_swizzled, gpu::BarrierType::WRITE);
		gpu::bindUniformBuffer(4, m_params[index], 0, 256);
		gpu::useProgram(m_program);
		gpu::dispatch((m_settings.source_width + 15) / 16, (m_settings.source_height + 15) / 16, 1);
		// GENERIC_READ includes COPY_SOURCE
		gpu::barrier(m_swizzled, gpu::BarrierType::READ);

		// copy to the texture of the slot, it is registered in NVENC, it has no UAV flag and starts and ends in COMMON
		ID3D12GraphicsCommandList* cmd_list = (ID3D12GraphicsCommandList*)gpu::getDX12CommandList();
		D3D12_RESOURCE_BARRIER to_copy = {};
		to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		to_copy.Transition.pResource = slot.input;
		to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		cmd_list->ResourceBarrier(1, &to_copy);
		D3D12_TEXTURE_COPY_LOCATION dst_location = {};
		dst_location.pResource = slot.input;
		dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION src_location = {};
		src_location.pResource = (ID3D12Resource*)gpu::getDX12Resource(m_swizzled);
		src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		// the video can be smaller than the source (even size), crop
		D3D12_BOX box = {};
		box.right = m_settings.width;
		box.bottom = m_settings.height;
		box.back = 1;
		cmd_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, &box);
		D3D12_RESOURCE_BARRIER to_common = to_copy;
		to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		cmd_list->ResourceBarrier(1, &to_common);

		MutexGuard api_guard(m_api_mutex);
		NV_ENC_MAP_INPUT_RESOURCE map_input = {};
		map_input.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
		map_input.registeredResource = slot.registered_input;
		NVENCSTATUS st = m_fn.nvEncMapInputResource(m_session, &map_input);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncMapInputResource (input)");
		slot.mapped_input = map_input.mappedResource;

		NV_ENC_MAP_INPUT_RESOURCE map_output = {};
		map_output.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
		map_output.registeredResource = slot.registered_output;
		st = m_fn.nvEncMapInputResource(m_session, &map_output);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncMapInputResource (output)");
		slot.mapped_output = map_output.mappedResource;

		slot.input_resource = {};
		slot.input_resource.version = NV_ENC_INPUT_RESOURCE_D3D12_VER;
		slot.input_resource.pInputBuffer = slot.mapped_input;
		slot.input_resource.inputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
		slot.input_resource.inputFencePoint.pFence = frame_fence;
		slot.input_resource.inputFencePoint.waitValue = frame_fence_value;
		slot.input_resource.inputFencePoint.bWait = 1;

		++slot.fence_value;
		slot.output_resource = {};
		slot.output_resource.version = NV_ENC_OUTPUT_RESOURCE_D3D12_VER;
		slot.output_resource.pOutputBuffer = slot.mapped_output;
		slot.output_resource.outputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
		slot.output_resource.outputFencePoint.pFence = slot.fence;
		slot.output_resource.outputFencePoint.signalValue = slot.fence_value;
		slot.output_resource.outputFencePoint.bSignal = 1;

		NV_ENC_PIC_PARAMS pic = {};
		pic.version = NV_ENC_PIC_PARAMS_VER;
		pic.inputWidth = m_settings.width;
		pic.inputHeight = m_settings.height;
		pic.inputBuffer = &slot.input_resource;
		pic.outputBitstream = &slot.output_resource;
		pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
		pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
		pic.inputTimeStamp = m_frame_index++;
		st = m_fn.nvEncEncodePicture(m_session, &pic);
		if (st != NV_ENC_SUCCESS) return fail(st, "nvEncEncodePicture");

		MutexGuard guard(m_mutex);
		++m_submitted;
		return true;
	}

	// Any thread except the render thread. Waits until the oldest submitted frame is encoded and writes it to the file.
	bool collect() {
		if (!m_session) return false;
		Slot* slot;
		{
			MutexGuard guard(m_mutex);
			if (m_collected >= m_submitted) return false;
			slot = &m_slots[m_collected % SLOT_COUNT];
		}

		bool ok = !m_failed;
		if (ok && slot->fence->GetCompletedValue() < slot->fence_value) {
			if (FAILED(slot->fence->SetEventOnCompletion(slot->fence_value, m_event)) || WaitForSingleObject(m_event, 10000) != WAIT_OBJECT_0) {
				setError("Timeout waiting for the encoder");
				ok = false;
			}
		}

		{
			MutexGuard api_guard(m_api_mutex);
			if (ok) {
				NV_ENC_LOCK_BITSTREAM lock = {};
				lock.version = NV_ENC_LOCK_BITSTREAM_VER;
				lock.outputBitstream = &slot->output_resource;
				const NVENCSTATUS st = m_fn.nvEncLockBitstream(m_session, &lock);
				if (st != NV_ENC_SUCCESS) ok = fail(st, "nvEncLockBitstream");
				else {
					const void* data = lock.bitstreamBufferPtr ? lock.bitstreamBufferPtr : slot->output_cpu;
					if (lock.bitstreamSizeInBytes > m_output_size) {
						setError("Encoded frame is bigger than the output buffer");
						ok = false;
					}
					else {
						m_sample.clear();
						if (m_mux.addPacket((const u8*)data, lock.bitstreamSizeInBytes, m_sample)) {
							ok = writeFile(m_sample.data(), (u32)m_sample.size());
							if (ok) ++m_frames_written;
						}
					}
					m_fn.nvEncUnlockBitstream(m_session, &slot->output_resource);
				}
			}
			if (slot->mapped_input) m_fn.nvEncUnmapInputResource(m_session, slot->mapped_input);
			if (slot->mapped_output) m_fn.nvEncUnmapInputResource(m_session, slot->mapped_output);
			slot->mapped_input = nullptr;
			slot->mapped_output = nullptr;
		}

		MutexGuard guard(m_mutex);
		++m_collected;
		return ok;
	}

	// patches the size of mdat and appends moov
	bool finishContainer() {
		OutputMemoryStream footer(m_allocator);
		if (!m_mux.writeFooter(footer)) return false;
		u8 size[8];
		const u64 mdat = m_mux.getMdatSize();
		for (u32 i = 0; i < 8; ++i) size[i] = u8(mdat >> (56 - 8 * i));
		if (!m_file.seek(m_mux.getMdatSizeOffset()) || !writeFile(size, 8) || !m_file.seekEnd()) return false;
		return writeFile(footer.data(), (u32)footer.size());
	}

	// Same thread as `collect`, after all frames were collected. Flushes the encoder and closes the file.
	bool finish() {
		bool ok = !m_failed && m_session != nullptr;
		if (ok) {
			MutexGuard api_guard(m_api_mutex);
			NV_ENC_PIC_PARAMS eos = {};
			eos.version = NV_ENC_PIC_PARAMS_VER;
			eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
			const NVENCSTATUS st = m_fn.nvEncEncodePicture(m_session, &eos);
			if (st != NV_ENC_SUCCESS) ok = fail(st, "nvEncEncodePicture (flush)");
		}
		if (m_file_open && !finishContainer()) ok = false;
		shutdown();
		return ok;
	}

	// main thread, after the renderer is done with the frames
	void destroy() {
		shutdown();
		DrawStream& stream = m_renderer.getEndFrameDrawStream();
		if (m_swizzled) stream.destroy(m_swizzled);
		if (m_program) stream.destroy(m_program);
		for (gpu::BufferHandle& params : m_params) {
			if (params) stream.destroy(params);
			params = gpu::INVALID_BUFFER;
		}
		m_swizzled = gpu::INVALID_TEXTURE;
		m_program = gpu::INVALID_PROGRAM;
	}

	void shutdown() {
		if (m_session) {
			for (Slot& slot : m_slots) {
				if (slot.mapped_input) m_fn.nvEncUnmapInputResource(m_session, slot.mapped_input);
				if (slot.mapped_output) m_fn.nvEncUnmapInputResource(m_session, slot.mapped_output);
				slot.mapped_input = nullptr;
				slot.mapped_output = nullptr;
				if (slot.registered_input) m_fn.nvEncUnregisterResource(m_session, slot.registered_input);
				if (slot.registered_output) m_fn.nvEncUnregisterResource(m_session, slot.registered_output);
				slot.registered_input = nullptr;
				slot.registered_output = nullptr;
			}
			m_fn.nvEncDestroyEncoder(m_session);
			m_session = nullptr;
		}
		for (Slot& slot : m_slots) {
			if (slot.output && slot.output_cpu) slot.output->Unmap(0, nullptr);
			slot.output_cpu = nullptr;
			releaseCom(slot.input);
			releaseCom(slot.output);
			releaseCom(slot.fence);
		}
		if (m_event) {
			CloseHandle(m_event);
			m_event = nullptr;
		}
		if (m_file_open) {
			m_file.close();
			m_file_open = false;
		}
		releaseCom(m_device);
		if (m_lib) {
			FreeLibrary(m_lib);
			m_lib = nullptr;
		}
	}

	IAllocator& m_allocator;
	Renderer& m_renderer;
	VideoEncoderSettings m_settings;
	char* m_error = nullptr;
	u32 m_error_capacity = 0;
	volatile bool m_failed = false;

	HMODULE m_lib = nullptr;
	NV_ENCODE_API_FUNCTION_LIST m_fn = {};
	ID3D12Device* m_device = nullptr;
	ID3D12Fence* m_reg_fence = nullptr;
	u64 m_reg_fence_value = 0;
	void* m_session = nullptr;
	HANDLE m_event = nullptr;
	os::OutputFile m_file;
	bool m_file_open = false;

	gpu::TextureHandle m_swizzled = gpu::INVALID_TEXTURE;	// BGRA8 copy of the frame, written by the compute pass
	gpu::ProgramHandle m_program = gpu::INVALID_PROGRAM;
	gpu::BufferHandle m_params[SLOT_COUNT] = {};			// constants of the compute pass, one per slot because the frames in flight read them
	Slot m_slots[SLOT_COUNT];
	u32 m_output_size = 0;
	Mutex m_mutex;				// protects the counters
	Mutex m_api_mutex;			// NVENC calls from the render thread and from the worker
	u64 m_submitted = 0;
	u64 m_collected = 0;
	u64 m_frame_index = 0;
	u64 m_frames_written = 0;
	Mp4Writer m_mux;
	OutputMemoryStream m_sample;		// the sample being written
};

// Main thread, after `finish`. Frees the encoder, accepts nullptr.
void destroyVideoEncoder(VideoEncoder* encoder) {
	if (!encoder) return;
	IAllocator& allocator = encoder->m_allocator;
	LUMIX_DELETE(allocator, encoder);
}

struct VideoRecorder final : StudioApp::GUIPlugin {
	enum Source : i32 {
		GAME_VIEW = 0,
		SCENE_VIEW = 1
	};

	enum class State {
		IDLE,
		RECORDING,
		STOPPING	// waiting for the encoder to write all submitted frames
	};

	struct EncodeThread final : Thread {
		EncodeThread(VideoRecorder& owner, IAllocator& allocator) : Thread(allocator), owner(owner) {}
		int task() override { owner.encodeLoop(); return 0; }
		VideoRecorder& owner;
	};

	// called at the end of every frame of every pipeline, it does nothing unless a frame of the recorded view was requested
	struct RenderHook final : RenderPlugin {
		explicit RenderHook(VideoRecorder& owner) : owner(owner) {}
		void renderEnd(RenderBufferHandle output, Pipeline& pipeline) override { owner.onRenderEnd(output, pipeline); }
		VideoRecorder& owner;
	};

	explicit VideoRecorder(StudioApp& app)
		: m_app(app)
		, m_output(app.getAllocator())
		, m_hook(*this)
	{
		m_output = "capture";
		m_renderer = (Renderer*)app.getEngine().getSystemManager().getSystem("renderer");
		if (m_renderer) m_renderer->addPlugin(m_hook);
		Settings& settings = app.getSettings();
		settings.registerOption("videorec_open", &m_is_open);
		settings.registerOption("videorec_output", &m_output);
		settings.registerOption("videorec_source", (i32*)&m_source);
		settings.registerOption("videorec_codec", &m_codec);
		settings.registerOption("videorec_width", &m_width);
		settings.registerOption("videorec_height", &m_height);
		settings.registerOption("videorec_fps", &m_fps);
		settings.registerOption("videorec_quality", &m_quality);
		settings.registerOption("videorec_preset", &m_preset);
	}

	~VideoRecorder() override {
		if (m_state == State::RECORDING) stop();
		// callbacks from the render thread still reference this object
		for (u32 i = 0; i < 500 && m_state != State::IDLE; ++i) {
			finishStopping();
			if (m_state != State::IDLE) os::sleep(10);
		}
		if (m_renderer) m_renderer->removePlugin(m_hook);
	}

	const char* getName() const override { return "videorec"; }

	GameView* getGameView() { return static_cast<GameView*>(m_app.getGUIPlugin("game_view")); }
	SceneView* getSceneView() { return static_cast<SceneView*>(m_app.getGUIPlugin("scene_view")); }

	bool isSourceAvailable() { return m_recording_source == SCENE_VIEW ? getSceneView() != nullptr : getGameView() != nullptr; }

	// true if a frame was requested and the recorded view did not render it yet (e.g. its window is hidden)
	bool hasPendingCapture() {
		MutexGuard guard(m_mutex);
		return m_capture_requested;
	}

	// Pipeline job, end of the frame of a pipeline. Queues the final image of the recorded view for encoding.
	void onRenderEnd(RenderBufferHandle output, Pipeline& pipeline) {
		{
			MutexGuard guard(m_mutex);
			if (!m_capture_requested || &pipeline != m_source_pipeline) return;
			m_capture_requested = false;
		}
		const IVec2 size = pipeline.getDisplaySize();
		gpu::TextureHandle texture = m_renderer->toTexture(output);
		// the size of the source changes if the window is resized while recording the scene view
		if (!texture || size != m_source_size) {
			MutexGuard guard(m_mutex);
			++m_dropped;
			--m_in_flight;
			return;
		}
		// the encoder records its commands to the command list of the renderer
		m_renderer->getDrawStream().pushLambda([this, texture]() { submitFrame(texture); });
	}

	// Render thread, the commands of the frame are being recorded.
	void submitFrame(gpu::TextureHandle texture) {
		const bool submitted = m_encoder->submit(texture);

		MutexGuard guard(m_mutex);
		if (!submitted) {
			m_failed = true;	// the encoder reports the reason
			--m_in_flight;
			return;
		}
		++m_pending;
		m_ready.signal();
	}

	void encodeLoop() {
		for (;;) {
			m_ready.wait();
			{
				MutexGuard guard(m_mutex);
				if (m_pending > 0) --m_pending;
				else if (m_finish_requested) break;
				else continue;
			}

			const bool ok = m_encoder->collect();
			MutexGuard guard(m_mutex);
			if (ok) ++m_frames_written;
			else m_failed = true;
			--m_in_flight;
		}
		const bool ok = m_encoder->finish();
		MutexGuard guard(m_mutex);
		if (!ok) m_failed = true;
	}

	bool isAbsolute(const char* path) const {
		if (!path[0]) return false;
		return path[0] == '/' || path[0] == '\\' || (path[1] == ':' && path[2] != '\0');
	}

	void start() {
		m_status = "";
		m_recording_source = m_source;
		GameView* game_view = getGameView();
		SceneView* scene_view = getSceneView();
		if (m_recording_source == GAME_VIEW && !game_view) {
			m_status = "Game view is not available";
			return;
		}
		if (m_recording_source == SCENE_VIEW && !scene_view) {
			m_status = "Scene view is not available";
			return;
		}
		if (m_output.length() == 0) {
			m_status = "Output file is empty";
			return;
		}

		VideoEncoderSettings settings;
		settings.codec = (VideoEncoderSettings::Codec)clamp(m_codec, 0, 2);
		// 4:2:0 chroma subsampling needs even dimensions
		settings.width = u32(clamp(m_width, 64, 8192)) & ~1u;
		settings.height = u32(clamp(m_height, 64, 8192)) & ~1u;
		IVec2 source_size((i32)settings.width, (i32)settings.height);
		if (m_recording_source == SCENE_VIEW) {
			// the scene view is recorded as it is in its window
			source_size = scene_view->getPipeline()->getDisplaySize();
			if (source_size.x < 64 || source_size.y < 64) {
				m_status = "Scene view is not visible, open the Scene View window";
				return;
			}
			settings.width = u32(source_size.x) & ~1u;
			settings.height = u32(source_size.y) & ~1u;
		}
		settings.source_width = (u32)source_size.x;
		settings.source_height = (u32)source_size.y;
		settings.fps = u32(clamp(m_fps, 1, 240));
		settings.quality = u32(clamp(m_quality, 1, 51));
		settings.preset = u32(clamp(m_preset, 1, 7));

		StaticString<MAX_PATH> path;
		if (!isAbsolute(m_output.c_str()) && m_app.getProjectDir()[0]) path.append(m_app.getProjectDir(), "/");
		path.append(m_output.c_str(), ".mp4");

		m_error[0] = '\0';
		Renderer* renderer = (Renderer*)m_app.getEngine().getSystemManager().getSystem("renderer");
		if (!renderer) {
			m_status = "Renderer is not available";
			return;
		}
		IAllocator& allocator = m_app.getAllocator();
		m_encoder = LUMIX_NEW(allocator, VideoEncoder)(allocator, *renderer);
		if (!m_encoder->init(settings, path, m_error, sizeof(m_error))) {
			LUMIX_DELETE(allocator, m_encoder);
			m_encoder = nullptr;
			m_status = m_error;
			logError("Video recorder: ", m_error);
			return;
		}

		m_source_size = source_size;
		{
			MutexGuard guard(m_mutex);
			m_source_pipeline = m_recording_source == SCENE_VIEW ? scene_view->getPipeline() : game_view->getPipeline();
			m_capture_requested = false;
		}
		m_soft_limit = VIDEO_ENCODER_MAX_IN_FLIGHT - 2;
		m_in_flight = 0;
		m_pending = 0;
		m_frames_written = 0;
		m_dropped = 0;
		m_failed = false;
		m_finish_requested = false;
		m_finish_sent = false;
		m_paused_by_us = false;
		m_recording_fps = settings.fps;
		m_recording_path = path;

		m_thread = LUMIX_NEW(m_app.getAllocator(), EncodeThread)(*this, m_app.getAllocator());
		if (!m_thread->create("videorec_encode", false)) {
			LUMIX_DELETE(m_app.getAllocator(), m_thread);
			m_thread = nullptr;
			m_encoder->finish();
			destroyVideoEncoder(m_encoder);
			m_encoder = nullptr;
			m_status = "Could not create the encoder thread";
			return;
		}

		if (m_recording_source == GAME_VIEW) {
			game_view->forceViewport(true, (int)settings.width, (int)settings.height);
			game_view->setForceRender(true);
		}
		m_app.getEngine().setFixedTimeDelta(1.f / settings.fps);
		m_state = State::RECORDING;
		logInfo("Video recorder: recording to ", path);
	}

	void stop() {
		if (m_state != State::RECORDING) return;

		Engine& engine = m_app.getEngine();
		engine.setFixedTimeDelta(0);
		if (m_paused_by_us) {
			engine.pause(false);
			m_paused_by_us = false;
		}

		if (m_recording_source == GAME_VIEW) {
			if (GameView* game_view = getGameView()) {
				game_view->forceViewport(false, 0, 0);
				game_view->setForceRender(false);
			}
		}

		{
			MutexGuard guard(m_mutex);
			if (m_capture_requested) {
				// not rendered yet, so the frame will not come
				m_capture_requested = false;
				--m_in_flight;
			}
			m_source_pipeline = nullptr;
		}
		m_state = State::STOPPING;
	}

	void finishStopping() {
		if (m_state != State::STOPPING) return;

		if (!m_finish_sent) {
			MutexGuard guard(m_mutex);
			if (m_in_flight != 0) return;
			m_finish_requested = true;
			m_finish_sent = true;
			m_ready.signal();
			return;
		}

		if (!m_thread->isFinished()) return;

		m_thread->destroy();
		LUMIX_DELETE(m_app.getAllocator(), m_thread);
		m_thread = nullptr;
		destroyVideoEncoder(m_encoder);
		m_encoder = nullptr;
		m_state = State::IDLE;

		MutexGuard guard(m_mutex);
		if (m_failed) {
			m_status = m_error[0] ? m_error : "Encoding failed";
			logError("Video recorder: ", m_status.data);
		}
		else {
			m_status = StaticString<MAX_PATH + 64>("Saved ", m_recording_path, " (", (i32)m_frames_written, " frames)");
			logInfo("Video recorder: ", m_status.data);
		}
		if (m_dropped > 0) logWarning("Video recorder: ", (i32)m_dropped, " frames were dropped");
	}

	void update(float) override {
		if (m_state == State::IDLE) return;
		if (m_state == State::STOPPING) {
			finishStopping();
			return;
		}

		u32 in_flight;
		bool failed;
		{
			MutexGuard guard(m_mutex);
			in_flight = m_in_flight;
			failed = m_failed;
		}
		if (failed || !isSourceAvailable()) {
			stop();
			return;
		}

		Engine& engine = m_app.getEngine();
		if (m_paused_by_us) {
			// wait until the encoder catches up and the view rendered the last request
			if (in_flight <= m_soft_limit / 2 && !hasPendingCapture()) {
				engine.pause(false);
				m_paused_by_us = false;
			}
			return;
		}

		// the view did not render since the last request (hidden window), do not let the simulation run away
		if (hasPendingCapture()) {
			engine.pause(true);
			m_paused_by_us = true;
			return;
		}

		{
			MutexGuard guard(m_mutex);
			++m_in_flight;
			++in_flight;
			m_capture_requested = true;
		}
		if (in_flight >= m_soft_limit) {
			engine.pause(true);
			m_paused_by_us = true;
		}
	}

	void onGUI() override {
		if (m_app.checkShortcut(m_toggle_ui, true)) m_is_open = !m_is_open;
		// also invoked programmatically, e.g. by the remote control plugin
		if (m_app.checkShortcut(m_record, true, true)) {
			if (m_state == State::IDLE) start();
			else stop();
		}
		if (!m_is_open) return;

		if (ImGui::Begin(ICON_FA_VIDEO "Video recorder##videorec", &m_is_open)) {
			const bool busy = m_state != State::IDLE;

			ImGui::BeginDisabled(busy);

			struct Preset { const char* name; i32 width; i32 height; i32 fps; i32 cq; i32 effort; };
			static const Preset PRESETS[] = {
				{ "Draft 720p@30", 1280, 720, 30, 32, 2 },
				{ "1080p@30", 1920, 1080, 30, 26, 5 },
				{ "HQ 1080p@60", 1920, 1080, 60, 22, 6 },
				{ "HQ 4K@60", 3840, 2160, 60, 24, 5 },
				// quality 44 and above looks the same, the bitrate stops growing
				{ "Max 4K@60", 3840, 2160, 60, 8, 7 },
			};
			const i32 custom_idx = (i32)lengthOf(PRESETS);
			if (m_preset_idx < 0) {
				// not chosen yet (e.g. settings were just loaded), pick the matching preset or custom
				m_preset_idx = custom_idx;
				for (i32 i = 0; i < custom_idx; ++i) {
					const Preset& p = PRESETS[i];
					if (p.width == m_width && p.height == m_height && p.fps == m_fps && p.cq == m_quality && p.effort == m_preset) m_preset_idx = i;
				}
			}
			const char* preview = m_preset_idx < custom_idx ? PRESETS[m_preset_idx].name : "Custom";
			if (ImGui::BeginCombo("Preset", preview)) {
				for (i32 i = 0; i <= custom_idx; ++i) {
					const char* name = i < custom_idx ? PRESETS[i].name : "Custom";
					if (ImGui::Selectable(name, m_preset_idx == i)) {
						m_preset_idx = i;
						if (i < custom_idx) {
							const Preset& p = PRESETS[i];
							m_width = p.width;
							m_height = p.height;
							m_fps = p.fps;
							m_quality = p.cq;
							m_preset = p.effort;
						}
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets resolution, frame rate, quality and encoder effort.\nThe codec and the output file are not changed.");
			const bool is_custom = m_preset_idx >= custom_idx;

			inputString("Output file", &m_output);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("File name without extension, .mp4 is added.\nRelative to the project directory unless it is an absolute path.");
			static const char* SOURCES[] = { "Game view", "Scene view" };
			ImGui::Combo("Source", (i32*)&m_source, SOURCES, (int)lengthOf(SOURCES));
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(m_source == SCENE_VIEW
					? "Scene view: recorded at the current size of its window.\nThe Scene View tab has to be visible and must not be resized while recording."
					: "Game view: rendered at the resolution below, its window can be closed.");
			}
			static const char* CODECS[] = { "AV1 (royalty free, RTX 40 series or newer)", "H.264", "HEVC" };
			ImGui::Combo("Codec", &m_codec, CODECS, (int)lengthOf(CODECS));
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("AV1 is royalty free and needs an RTX 40 series GPU or newer.\nH.264 and HEVC work on older GPUs. All are saved to an MP4 file.");
			}
			if (is_custom) {
				// the scene view is recorded in the size of its window
				ImGui::BeginDisabled(m_source == SCENE_VIEW);
				ImGui::InputInt("Width", &m_width, 2, 64);
				ImGui::InputInt("Height", &m_height, 2, 64);
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("Resolution of the game view video, rounded down to an even number.\nIgnored for the scene view, it is recorded at the size of its window.");
				}
				ImGui::InputInt("FPS", &m_fps);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Frame rate of the video. The game is simulated with a fixed step of 1 / FPS\nwhile recording, so it is smooth even if Studio renders slower.");
				}
				// NVENC's constant quality level (CQ) is "lower is better", the slider shows the opposite
				i32 quality = 52 - clamp(m_quality, 1, 51);
				if (ImGui::SliderInt("Quality", &quality, 1, 51)) m_quality = 52 - quality;
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("How much detail the encoder keeps. Higher is a better picture and a bigger file.\n(NVENC constant quality level %d, lower is better there)", m_quality);
				}
				ImGui::SliderInt("Encoder effort", &m_preset, 1, 7);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Not the target quality (that is Quality above).\nHigher spends more GPU time to compress better at the same quality,\nlower is faster. 1 is the fastest, 7 is the slowest.");
				}
			}
			else {
				ImGui::TextDisabled("%dx%d, %d fps, quality %d, encoder effort %d", m_width, m_height, m_fps, 52 - m_quality, m_preset);
			}
			m_width = clamp(m_width, 64, 8192);
			m_height = clamp(m_height, 64, 8192);
			m_fps = clamp(m_fps, 1, 240);
			ImGui::EndDisabled();

			if (m_state == State::IDLE) {
				if (ImGui::Button(ICON_FA_CIRCLE " Start recording")) start();
				if (!m_app.getWorldEditor().isGameMode()) ImGui::TextUnformatted("Start the game to record gameplay.");
			}
			else if (m_state == State::RECORDING) {
				if (ImGui::Button(ICON_FA_STOP " Stop recording")) stop();
			}
			else {
				ImGui::TextUnformatted("Finishing...");
			}

			if (m_state != State::IDLE) {
				MutexGuard guard(m_mutex);
				const float seconds = m_recording_fps ? float(m_frames_written) / float(m_recording_fps) : 0.f;
				ImGui::Text("%d frames, %.1f s, %d in queue", (int)m_frames_written, seconds, (int)m_in_flight);
				if (m_dropped) ImGui::Text("%d frames dropped", (int)m_dropped);
			}
			if (!m_status.empty()) ImGui::TextWrapped("%s", m_status.data);
		}
		ImGui::End();
	}

	StudioApp& m_app;
	State m_state = State::IDLE;
	bool m_is_open = false;

	// settings
	String m_output;
	Source m_source = GAME_VIEW;
	i32 m_codec = 0;
	i32 m_width = 1920;
	i32 m_height = 1080;
	i32 m_fps = 60;
	i32 m_quality = 28;
	i32 m_preset = 5;
	i32 m_preset_idx = -1;	// UI only, index into the preset list, the last entry is custom

	// session, the mutex protects everything from here to `m_finish_requested`
	Mutex m_mutex;
	Semaphore m_ready{0, 1 << 20};
	u32 m_in_flight = 0;	// requested but not yet written to the file
	u32 m_pending = 0;		// submitted to the encoder but not yet collected
	u64 m_frames_written = 0;
	u64 m_dropped = 0;
	bool m_failed = false;
	bool m_finish_requested = false;

	Source m_recording_source = GAME_VIEW;	// source of the current recording, `m_source` can change in the UI
	IVec2 m_source_size = IVec2(0);			// size of the frames the source delivers
	Renderer* m_renderer = nullptr;
	RenderHook m_hook;
	Pipeline* m_source_pipeline = nullptr;	// pipeline of the recorded view, protected by the mutex
	bool m_capture_requested = false;		// the next frame of the source pipeline has to be encoded, protected by the mutex
	u32 m_soft_limit = 0;
	u32 m_recording_fps = 0;
	bool m_finish_sent = false;
	bool m_paused_by_us = false;
	VideoEncoder* m_encoder = nullptr;
	EncodeThread* m_thread = nullptr;
	StaticString<MAX_PATH> m_recording_path;
	char m_error[256] = {};
	StaticString<MAX_PATH + 64> m_status;

	Action m_toggle_ui{"Video recorder", "Video recorder", "Toggle UI", "videorec_toggle_ui", "", Action::WINDOW};
	Action m_record{"Video recorder", "Record", "Start or stop recording", "videorec_record", ICON_FA_VIDEO, Action::NORMAL};
};

} // namespace Lumix

LUMIX_STUDIO_ENTRY(videorec) {
	auto* plugin = LUMIX_NEW(app.getAllocator(), Lumix::VideoRecorder)(app);
	app.addPlugin((Lumix::StudioApp::GUIPlugin&)*plugin);
	return nullptr;
}

LUMIX_PLUGIN_ENTRY(videorec) {
	return nullptr;
}
