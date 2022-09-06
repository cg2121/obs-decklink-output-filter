#include "obs-module.h"

struct decklink_output_filter_context {
	obs_output_t *output;
	obs_source_t *source;

	video_t *video;
	obs_view_t *view;

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

	obs_view_remove(filter->view);
	obs_view_set_source(filter->view, 0, NULL);
	video_output_close(filter->video);

	obs_output_stop(filter->output);
	obs_output_release(filter->output);

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

	obs_data_t *settings = obs_source_get_settings(filter->source);
	const char *hash = obs_data_get_string(settings, "device_hash");
	int mode_id = (int)obs_data_get_int(settings, "mode_id");
	obs_data_release(settings);

	if (!hash || !*hash || !mode_id)
		return;

	if (!filter->view)
		filter->view = obs_view_create();

	obs_view_set_source(filter->view, 0,
			    obs_filter_get_parent(filter->source));

	if (!filter->video)
		filter->video = obs_view_add(filter->view);

	filter->output = obs_output_create(
		"decklink_output", "decklink_filter_output", settings, NULL);

	obs_output_set_media(filter->output, filter->video, obs_get_audio());

	bool started = obs_output_start(filter->output);
	filter->active = true;

	if (!started)
		decklink_output_filter_stop(filter);
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

	decklink_output_filter_update(source, settings);
	return filter;
}

static void decklink_output_filter_destroy(void *data)
{
	struct decklink_output_filter_context *filter = data;

	decklink_output_filter_stop(filter);
	obs_view_destroy(filter->view);
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

struct obs_source_info decklink_output_filter = {
	.id = "decklink_output_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = decklink_output_filter_get_name,
	.create = decklink_output_filter_create,
	.destroy = decklink_output_filter_destroy,
	.update = decklink_output_filter_update,
	.get_properties = decklink_output_filter_properties,
};

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
