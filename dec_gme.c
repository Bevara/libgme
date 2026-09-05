/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / chiptune decoder filter, based on
 *  game-music-emu (https://github.com/libgme/game-music-emu). The library
 *  emulates the sound hardware of the machine the music was written for, so a
 *  file is a register log or a driver plus its data, not samples: NSF (NES),
 *  GBS (Game Boy), SPC (SNES), VGM (Mega Drive and many chips), AY (ZX
 *  Spectrum / Amstrad YM), GYM, HES (PC Engine), KSS (MSX) and SAP (Atari).
 *
 *  Only the first track of a multi-track file is rendered.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <gme/gme.h>

#define GMEDEC_SAMPLE_RATE 44100
#define GMEDEC_CHANNELS 2
/* Most chiptunes loop forever; rendering is capped and faded out instead. */
#define GMEDEC_MAX_SECONDS 120
#define GMEDEC_FADE_MS 110000
#define GMEDEC_CHUNK_FRAMES 4096

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_GMEDecCtx;

/* game-music-emu keeps one descriptor object per emulator, and reaches them
 * through a table the linker does not follow into the static library. With
 * nothing naming them, those members stay out of the link and the side module
 * ends up importing gme_nsf_type & co. from a host that has never heard of
 * them - it then fails to instantiate and the filter never registers at all.
 * Naming them here is what pulls the emulators in. */
extern gme_type_t const gme_ay_type, gme_gbs_type, gme_gym_type, gme_hes_type;
extern gme_type_t const gme_kss_type, gme_nsf_type, gme_nsfe_type, gme_sap_type;
extern gme_type_t const gme_spc_type, gme_vgm_type, gme_vgz_type;

/* Assigned at run time rather than in a static initializer: under -fPIC these
 * addresses are not compile-time constants. */
static void gmedec_link_emulators(void)
{
	volatile gme_type_t sink;
	sink = gme_ay_type;   sink = gme_gbs_type;  sink = gme_gym_type;
	sink = gme_hes_type;  sink = gme_kss_type;  sink = gme_nsf_type;
	sink = gme_nsfe_type; sink = gme_sap_type;  sink = gme_spc_type;
	sink = gme_vgm_type;  sink = gme_vgz_type;
	(void)sink;
}

static GF_Err gmedec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_GMEDecCtx *ctx = (GF_GMEDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(GMEDEC_SAMPLE_RATE));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(GMEDEC_SAMPLE_RATE));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(GMEDEC_CHANNELS));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT(GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

	return GF_OK;
}

static Bool gmedec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_GMEDecCtx *ctx = (GF_GMEDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err gmedec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, max_samples, pcm_used = 0, pcm_alloc, chunk_samples;
	Music_Emu *emu = NULL;
	gme_err_t err;
	s16 *pcm = NULL;
	GF_GMEDecCtx *ctx = (GF_GMEDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	gmedec_link_emulators();
	err = gme_open_data(data, (long)size, &emu, GMEDEC_SAMPLE_RATE);
	gf_filter_pid_drop_packet(ctx->ipid);
	if (err || !emu)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[GMEDec] %s\n", err ? err : "unsupported chiptune file"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (gme_start_track(emu, 0))
	{
		gme_delete(emu);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[GMEDec] Failed to start track 0\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gme_set_fade(emu, GMEDEC_FADE_MS);

	max_samples = (u32)GMEDEC_MAX_SECONDS * GMEDEC_SAMPLE_RATE * GMEDEC_CHANNELS;
	chunk_samples = GMEDEC_CHUNK_FRAMES * GMEDEC_CHANNELS;
	pcm_alloc = GMEDEC_SAMPLE_RATE * GMEDEC_CHANNELS;
	pcm = (s16 *)gf_malloc(pcm_alloc * sizeof(s16));

	while (pcm && (pcm_used < max_samples) && !gme_track_ended(emu))
	{
		if (pcm_used + chunk_samples > pcm_alloc)
		{
			s16 *bigger;
			pcm_alloc *= 2;
			bigger = (s16 *)gf_realloc(pcm, pcm_alloc * sizeof(s16));
			if (!bigger)
				break;
			pcm = bigger;
		}
		if (gme_play(emu, (int)chunk_samples, pcm + pcm_used))
			break;
		pcm_used += chunk_samples;
	}
	gme_delete(emu);

	if (!pcm || !pcm_used)
	{
		if (pcm)
			gf_free(pcm);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, pcm_used * (u32)sizeof(s16), &output);
	if (!dst_pck)
	{
		gf_free(pcm);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, pcm, pcm_used * sizeof(s16));
	gf_free(pcm);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_set_duration(dst_pck, pcm_used / GMEDEC_CHANNELS);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void gmedec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability GMEDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "nsf|nsfe|gbs|spc|vgm|ay|gym|hes|kss|sap"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/x-nsf|audio/x-spc|audio/x-vgm|audio/x-gbs"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister GMEDecoderRegister = {
	.name = "gmedec",
	GF_FS_SET_DESCRIPTION("Chiptune decoder (NSF, GBS, SPC, VGM, ...)")
		GF_FS_SET_HELP("This filter renders video game music files (NSF, GBS, SPC, VGM, AY, GYM, HES, KSS, SAP) to PCM by emulating the original sound hardware, using game-music-emu.")
			.private_size = sizeof(GF_GMEDecCtx),
	SETCAPS(GMEDecCaps),
	.configure_pid = gmedec_configure_pid,
	.process = gmedec_process,
	.process_event = gmedec_process_event,
	.finalize = gmedec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE gmedec_register(GF_FilterSession *session)
{
	return &GMEDecoderRegister;
}

#include "filter_register.h"
/* Priority 101 so this runs before game-music-emu's own C++ static
 * constructors: if one of those traps, the whole ctor chain stops and the
 * filter would never be registered at all. */
__attribute__((constructor(101)))
void register_gmedec(void) {
    gf_filter_auto_register("gmedec", gmedec_register);
}
