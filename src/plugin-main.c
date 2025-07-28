#include "obs.h"
#include "obs-module.h"
#include "plugin-support.h"

struct decklink_output_filter_context {
	obs_output_t *output;
	obs_source_t *source;
	obs_canvas_t *canvas;

	obs_property_t *button;

	bool active;
};

static const char *decklink_output_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DecklinkOutput");
}

static void decklink_output_filter_stop(void *data)
{
	struct decklink_output_filter_context *filter = data;

	if (!filter->active)
		return;

	obs_output_stop(filter->output);
	obs_canvas_release(filter->canvas);
	obs_output_release(filter->output);

	filter->active = false;

	if (filter->button)
		obs_property_set_description(filter->button, obs_module_text("Start"));
}

static void decklink_output_filter_start(void *data, obs_data_t *settings)
{
	struct decklink_output_filter_context *filter = data;

	if (filter->active)
		return;

	if (!obs_source_enabled(filter->source)) {
		obs_log(LOG_ERROR, "Filter not enabled");
		return;
	}

	filter->output = obs_output_create("decklink_output", "decklink_filter_output", settings, NULL);

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	filter->canvas = obs_canvas_create_private(NULL, &ovi, DEVICE);

	obs_canvas_set_channel(filter->canvas, 0, obs_filter_get_parent(filter->source));

	obs_output_set_media(filter->output, obs_canvas_get_video(filter->canvas), obs_get_audio());

	bool started = obs_output_start(filter->output);

	obs_source_inc_showing(obs_filter_get_parent(filter->source));

	filter->active = true;

	if (!started) {
		obs_log(LOG_ERROR, "Filter failed to start");
		decklink_output_filter_stop(filter);
		return;
	}

	if (filter->button)
		obs_property_set_description(filter->button, obs_module_text("Stop"));

	obs_log(LOG_INFO, "Filter started successfully");
}

static void decklink_output_filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(settings);
}

static void set_filter_enabled(void *data, calldata_t *calldata)
{
	struct decklink_output_filter_context *filter = data;

	bool enable = calldata_bool(calldata, "enabled");
	obs_data_t *settings = obs_source_get_settings(filter->source);
	obs_data_release(settings);

	bool auto_start = obs_data_get_bool(settings, "auto_start");

	if (enable && auto_start)
		decklink_output_filter_start(filter, settings);
	else
		decklink_output_filter_stop(filter);
}

static void *decklink_output_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct decklink_output_filter_context *filter = bzalloc(sizeof(struct decklink_output_filter_context));
	filter->source = source;
	filter->active = false;
	filter->button = NULL;

	signal_handler_t *sh = obs_source_get_signal_handler(filter->source);
	signal_handler_connect(sh, "enable", set_filter_enabled, filter);

	bool auto_start = obs_data_get_bool(settings, "auto_start");

	if (auto_start)
		decklink_output_filter_start(filter, settings);

	return filter;
}

static void decklink_output_filter_destroy(void *data)
{
	struct decklink_output_filter_context *filter = data;
	filter->button = NULL;

	decklink_output_filter_stop(filter);

	signal_handler_t *sh = obs_source_get_signal_handler(filter->source);
	signal_handler_disconnect(sh, "enable", set_filter_enabled, filter);

	bfree(filter);
}

static bool button_cb(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);

	struct decklink_output_filter_context *filter = data;

	obs_data_t *settings = obs_source_get_settings(filter->source);
	obs_data_release(settings);

	obs_property_set_enabled(property, false);

	if (!filter->active)
		decklink_output_filter_start(filter, settings);
	else
		decklink_output_filter_stop(filter);

	obs_property_set_enabled(property, true);

	return true;
}

static obs_properties_t *decklink_output_filter_properties(void *data)
{
	struct decklink_output_filter_context *filter = data;

	obs_properties_t *props = obs_get_output_properties("decklink_output");
	filter->button = obs_properties_add_button2(props, "Button",
						    filter->active ? obs_module_text("Stop") : obs_module_text("Start"),
						    button_cb, filter);

	return props;
}

struct obs_source_info decklink_output_filter = {.id = "decklink_output_filter",
						 .type = OBS_SOURCE_TYPE_FILTER,
						 .output_flags = OBS_SOURCE_VIDEO,
						 .get_name = decklink_output_filter_get_name,
						 .create = decklink_output_filter_create,
						 .destroy = decklink_output_filter_destroy,
						 .update = decklink_output_filter_update,
						 .get_properties = decklink_output_filter_properties,};

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
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_register_source(&decklink_output_filter);
}
