#include "obs-module.h"
#include "plugin-support.h"
#include "media-io/video-io.h"
#include "media-io/video-frame.h"
#include "util/platform.h"

#define STAGE_BUFFER_COUNT 3

struct decklink_output_filter_context {
	obs_output_t *output;
	obs_source_t *source;

	bool active;

	video_t *video_queue;
	gs_texrender_t *texrender_premultiplied;
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurfaces[STAGE_BUFFER_COUNT];
	bool surf_written[STAGE_BUFFER_COUNT];
	size_t stage_index;
	uint8_t *video_data;
	uint32_t video_linesize;
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

	uint32_t width = 0;
	uint32_t height = 0;
	gs_texture_t *tex = NULL;

	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (!parent)
		return;

	width = obs_source_get_base_width(parent);
	height = obs_source_get_base_height(parent);

	gs_texrender_t *const texrender_premultiplied =
		filter->texrender_premultiplied;
	if (!gs_texrender_begin(texrender_premultiplied, width, height))
		return;

	struct vec4 background;
	vec4_zero(&background);

	gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	obs_source_skip_video_filter(filter->source);

	gs_blend_state_pop();
	gs_texrender_end(texrender_premultiplied);

	tex = gs_texrender_get_texture(texrender_premultiplied);

	const struct video_scale_info *const conversion =
		obs_output_get_video_conversion(filter->output);
	const uint32_t scaled_width = conversion->width;
	const uint32_t scaled_height = conversion->height;

	if (!gs_texrender_begin(filter->texrender, scaled_width, scaled_height))
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_enable_blending(false);

	gs_effect_t *const effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"),
				   tex);
	while (gs_effect_loop(effect, "DrawAlphaDivide")) {
		gs_draw_sprite(tex, 0, 0, 0);
	}

	gs_enable_blending(true);
	gs_enable_framebuffer_srgb(previous);

	gs_texrender_end(filter->texrender);

	const size_t write_stage_index = filter->stage_index;
	gs_stage_texture(filter->stagesurfaces[write_stage_index],
			 gs_texrender_get_texture(filter->texrender));
	filter->surf_written[write_stage_index] = true;

	const size_t read_stage_index =
		(write_stage_index + 1) % STAGE_BUFFER_COUNT;
	if (filter->surf_written[read_stage_index]) {
		struct video_frame output_frame;
		if (video_output_lock_frame(filter->video_queue, &output_frame,
					    1, os_gettime_ns())) {
			gs_stagesurf_t *const read_surf =
				filter->stagesurfaces[read_stage_index];
			if (gs_stagesurface_map(read_surf, &filter->video_data,
						&filter->video_linesize)) {
				uint32_t linesize = output_frame.linesize[0];
				for (uint32_t i = 0; i < scaled_height; i++) {
					uint32_t dst_offset = linesize * i;
					uint32_t src_offset =
						filter->video_linesize * i;
					memcpy(output_frame.data[0] +
						       dst_offset,
					       filter->video_data + src_offset,
					       linesize);
				}

				gs_stagesurface_unmap(read_surf);
				filter->video_data = NULL;
			}

			video_output_unlock_frame(filter->video_queue);
		}
	}

	filter->stage_index = read_stage_index;
}

static void decklink_output_filter_stop(void *data)
{
	struct decklink_output_filter_context *filter = data;

	if (!filter->active)
		return;

	obs_output_stop(filter->output);
	obs_output_release(filter->output);

	obs_remove_main_render_callback(render_preview_source, filter);

	obs_enter_graphics();

	for (size_t i = 0; i < STAGE_BUFFER_COUNT; i++) {
		gs_stagesurface_destroy(filter->stagesurfaces[i]);
		filter->stagesurfaces[i] = NULL;
	}

	gs_texrender_destroy(filter->texrender);
	filter->texrender = NULL;
	gs_texrender_destroy(filter->texrender_premultiplied);
	filter->texrender_premultiplied = NULL;
	obs_leave_graphics();

	video_output_close(filter->video_queue);

	filter->active = false;
}

static void decklink_output_filter_start(void *data, obs_data_t *settings)
{
	struct decklink_output_filter_context *filter = data;

	if (filter->active)
		decklink_output_filter_stop(filter);

	if (!obs_source_enabled(filter->source)) {
		blog(LOG_ERROR, "Filter not enabled");
		return;
	}

	filter->output = obs_output_create(
		"decklink_output", "decklink_filter_output", settings, NULL);

	const struct video_scale_info *const conversion =
		obs_output_get_video_conversion(filter->output);

	if (!conversion) {
		obs_output_release(filter->output);
		return;
	}

	const uint32_t width = conversion->width;
	const uint32_t height = conversion->height;

	obs_enter_graphics();
	filter->texrender_premultiplied =
		gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	filter->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	for (size_t i = 0; i < STAGE_BUFFER_COUNT; i++)
		filter->stagesurfaces[i] =
			gs_stagesurface_create(width, height, GS_BGRA);
	obs_leave_graphics();

	for (size_t i = 0; i < STAGE_BUFFER_COUNT; i++)
		filter->surf_written[i] = false;

	filter->stage_index = 0;

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
	vi.range = VIDEO_RANGE_FULL;
	vi.name = obs_source_get_name(filter->source);

	video_output_open(&filter->video_queue, &vi);

	obs_add_main_render_callback(render_preview_source, filter);

	obs_output_set_media(filter->output, filter->video_queue,
			     obs_get_audio());

	bool started = obs_output_start(filter->output);

	filter->active = true;

	if (!started) {
		blog(LOG_ERROR, "Filter failed to start");
		decklink_output_filter_stop(filter);
	}

	blog(LOG_ERROR, "Filter started successfully");
}

static void decklink_output_filter_update(void *data, obs_data_t *settings)
{
	struct decklink_output_filter_context *filter = data;
	decklink_output_filter_start(filter, settings);
}

static void set_filter_enabled(void *data, calldata_t *calldata)
{
	struct decklink_output_filter_context *filter = data;

	bool enable = calldata_bool(calldata, "enabled");
	obs_data_t *settings = obs_source_get_settings(filter->source);
	obs_data_release(settings);

	if (enable)
		decklink_output_filter_start(filter, settings);
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

	obs_source_update(source, settings);
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

	if (filter->texrender_premultiplied)
		gs_texrender_reset(filter->texrender_premultiplied);
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
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)",
		PLUGIN_VERSION);
	obs_register_source(&decklink_output_filter);
}
