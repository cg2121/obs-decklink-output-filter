#include <obs-module.h>
#include "plugin-macros.generated.h"
#include <obs.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>

struct decklink_output_filter_context {
	obs_output_t *output;
	obs_source_t *source;

	video_t *video_output;
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;

	bool active;
};

static const char *decklink_output_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DecklinkOutput");
}

static void render_preview_source(void *data, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);

	struct decklink_output_filter_context *filter = data;

	uint32_t width = gs_stagesurface_get_width(filter->stagesurface);
	uint32_t height = gs_stagesurface_get_height(filter->stagesurface);

	if (!gs_texrender_begin(filter->texrender, width, height))
		return;

	struct vec4 background;
	vec4_zero(&background);

	gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	obs_source_skip_video_filter(filter->source);

	gs_blend_state_pop();
	gs_texrender_end(filter->texrender);

	struct video_frame output_frame;
	if (!video_output_lock_frame(filter->video_output, &output_frame, 1,
				     obs_get_video_frame_time()))
		return;

	gs_stage_texture(filter->stagesurface,
			 gs_texrender_get_texture(filter->texrender));

	uint8_t *video_data;
	uint32_t video_linesize;
	if (!gs_stagesurface_map(filter->stagesurface, &video_data,
				 &video_linesize))
		return;

	uint32_t linesize = output_frame.linesize[0];

	for (uint32_t i = 0; i < height; i++) {
		uint32_t dst_offset = linesize * i;
		uint32_t src_offset = video_linesize * i;
		memcpy(output_frame.data[0] + dst_offset,
		       video_data + src_offset, linesize);
	}

	gs_stagesurface_unmap(filter->stagesurface);
	video_output_unlock_frame(filter->video_output);
}

static void decklink_output_filter_stop(void *data)
{
	struct decklink_output_filter_context *filter = data;

	if (!filter->active)
		return;

	obs_remove_main_render_callback(render_preview_source, filter);

	obs_output_stop(filter->output);
	obs_output_release(filter->output);

	obs_enter_graphics();
	gs_stagesurface_destroy(filter->stagesurface);
	gs_texrender_destroy(filter->texrender);
	obs_leave_graphics();

	video_output_close(filter->video_output);

	filter->active = false;
}

static void decklink_output_filter_start(void *data)
{
	struct decklink_output_filter_context *filter = data;

	if (filter->active)
		return;

	filter->active = false;

	if (!obs_source_enabled(filter->source))
		return;

	obs_source_t *parent = obs_filter_get_target(filter->source);
	uint32_t width = obs_source_get_base_width(parent);
	uint32_t height = obs_source_get_base_height(parent);

	if (!width || !height)
		return;

	obs_data_t *settings = obs_source_get_settings(filter->source);
	const char *hash = obs_data_get_string(settings, "device_hash");
	int mode_id = obs_data_get_int(settings, "mode_id");
	obs_data_release(settings);

	if (!hash || !*hash || !mode_id)
		return;

	filter->output = obs_output_create(
		"decklink_output", "decklink_filter_output", settings, NULL);

	obs_enter_graphics();
	filter->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	filter->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
	obs_leave_graphics();

	const struct video_output_info *main_voi =
		video_output_get_info(obs_get_video());

	struct video_output_info vi = {0};
	vi.format = VIDEO_FORMAT_BGRA;
	vi.width = width;
	vi.height = height;
	vi.fps_den = main_voi->fps_den;
	vi.fps_num = main_voi->fps_num;
	vi.cache_size = 16;
	vi.colorspace = main_voi->colorspace;
	vi.range = main_voi->range;
	vi.name = obs_source_get_name(filter->source);

	video_output_open(&filter->video_output, &vi);
	obs_output_set_media(filter->output, filter->video_output,
			     obs_get_audio());

	bool started = obs_output_start(filter->output);
	filter->active = true;

	if (!started)
		decklink_output_filter_stop(filter);
	else
		obs_add_main_render_callback(render_preview_source, filter);
}

static void decklink_output_filter_update(void *data, obs_data_t *settings)
{
	struct decklink_output_filter_context *filter = data;
	decklink_output_filter_stop(filter);
	decklink_output_filter_start(filter);

	UNUSED_PARAMETER(settings);
}

static void set_filter_enabled(void *data, calldata_t *calldata)
{
	struct decklink_output_filter_context *filter = data;

	bool enable = calldata_bool(calldata, "enabled");

	if (enable)
		decklink_output_filter_start(filter);
	else
		decklink_output_filter_stop(filter);
}

static void *decklink_output_filter_create(obs_data_t *settings,
					   obs_source_t *source)
{
	struct decklink_output_filter_context *filter =
		bzalloc(sizeof(struct decklink_output_filter_context));
	filter->source = source;
	filter->active = false;

	signal_handler_t *sh = obs_source_get_signal_handler(filter->source);
	signal_handler_connect(sh, "enable", set_filter_enabled, filter);

	decklink_output_filter_update(filter, settings);

	return filter;
}

static void decklink_output_filter_destroy(void *data)
{
	struct decklink_output_filter_context *filter = data;
	decklink_output_filter_stop(filter);

	bfree(filter);
}

static obs_properties_t *decklink_output_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_get_output_properties("decklink_output");
	obs_property_t *prop = obs_properties_get(props, "auto_start");
	obs_property_set_visible(prop, false);

	return props;
}

void decklink_output_filter_tick(void *data, float sec)
{
	struct decklink_output_filter_context *filter = data;

	if (filter->texrender)
		gs_texrender_reset(filter->texrender);

	UNUSED_PARAMETER(sec);
}

struct obs_source_info decklink_output_filter = {
	.id = "decklink_output_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = decklink_output_filter_get_name,
	.create = decklink_output_filter_create,
	.destroy = decklink_output_filter_destroy,
	.update = decklink_output_filter_update,
	.get_properties = decklink_output_filter_properties,
	.video_tick = decklink_output_filter_tick};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("decklink-output-filter", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Decklink Output Filter";
}

bool obs_module_load(void)
{
	return true;
}

void obs_module_post_load(void)
{
	obs_register_source(&decklink_output_filter);
}
