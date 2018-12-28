#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <../UI/obs-frontend-api/obs-frontend-api.h>
#include <obs-scene.h>
#include "replay.h"

#define blog(log_level, format, ...) \
	blog(log_level, "[replay_source: '%s'] " format, \
			obs_source_get_name(context->source), ##__VA_ARGS__)

#define debug(format, ...) \
	blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) \
	blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) \
	blog(LOG_WARNING, format, ##__VA_ARGS__)

#define VISIBILITY_ACTION_RESTART 0
#define VISIBILITY_ACTION_PAUSE 1
#define VISIBILITY_ACTION_CONTINUE 2
#define VISIBILITY_ACTION_NONE 3

#define END_ACTION_HIDE 0
#define END_ACTION_PAUSE 1
#define END_ACTION_LOOP 2

struct replay_source {
	obs_source_t  *source;
	obs_source_t  *source_filter;
	obs_source_t  *source_audio_filter;
	char          *source_name;
	char          *source_audio_name;
	long          duration;
	int           speed_percent;
	bool          backward;
	bool          backward_start;
	int           visibility_action;
	int           end_action;
	char          *next_scene_name;
	obs_hotkey_id replay_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id pause_hotkey;
	obs_hotkey_id faster_hotkey;
	obs_hotkey_id slower_hotkey;
	obs_hotkey_id normal_speed_hotkey;
	obs_hotkey_id half_speed_hotkey;
	obs_hotkey_id trim_front_hotkey;
	obs_hotkey_id trim_end_hotkey;
	obs_hotkey_id trim_reset_hotkey;
	obs_hotkey_id backward_hotkey;
	uint64_t      first_frame_timestamp;
	uint64_t      start_timestamp;
	uint64_t      last_frame_timestamp;
	uint64_t      previous_frame_timestamp;
	uint64_t      pause_timestamp;
	int64_t       trim_front;
	int64_t       trim_end;
	int64_t       start_delay;
	struct obs_source_audio audio;

	bool          play;
	bool          restart;
	bool          active;
	bool          end;
	
	/* contains struct obs_source_frame* */
	struct obs_source_frame**        video_frames;
	uint64_t                         video_frame_count;
	uint64_t                         video_frame_position;

	/* stores the audio data */
	struct obs_audio_data*         audio_frames;
	uint64_t                       audio_frame_count;
	uint64_t                       audio_frame_position;
	struct obs_audio_data          audio_output;

	pthread_mutex_t    mutex;
};


static const char *replay_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplayInput");
}

static void EnumFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_id(filter);
	if ((strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 || strcmp(REPLAY_FILTER_ID, id) == 0) && strcmp(filterName, sourceName) == 0)
		c->source_filter = filter;

}
static void EnumAudioFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_id(filter);
	if (strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 && strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;

}
static void EnumAudioVideoFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_id(filter);
	if ((strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 || strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 || strcmp(REPLAY_FILTER_ID, id) == 0) && strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;
}

static void replay_backward_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(pressed){
		const int64_t time = obs_get_video_frame_time();
		if(c->pause_timestamp)
		{
			c->start_timestamp += time - c->pause_timestamp;
			c->pause_timestamp = time;
		}
		c->backward = !c->backward;
		const int64_t duration = ((int64_t)c->last_frame_timestamp - (int64_t)c->first_frame_timestamp) * (int64_t)100 / (int64_t)c->speed_percent;
		int64_t play_duration = time - c->start_timestamp;
		while(play_duration > duration)
		{
			play_duration -= duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
}

static void replay_source_update(void *data, obs_data_t *settings)
{
	struct replay_source *context = data;
	const char *source_name = obs_data_get_string(settings, SETTING_SOURCE);
	if (context->source_name){
		if(strcmp(context->source_name, source_name) != 0){
			obs_source_t *s = obs_get_source_by_name(context->source_name);
			if(s){
				do{
					context->source_filter = NULL;
					obs_source_enum_filters(s, EnumFilter, data);
					if(context->source_filter)
					{
						obs_source_filter_remove(s,context->source_filter);
					}
				}while(context->source_filter);
				obs_source_release(s);
			}
			bfree(context->source_name);
			context->source_name = bstrdup(source_name);
		}
	}else{
		context->source_name = bstrdup(source_name);
	}
	const char *source_audio_name = obs_data_get_string(settings, SETTING_SOURCE_AUDIO);
	if (context->source_audio_name){
		if(strcmp(context->source_audio_name, source_audio_name) != 0){
			obs_source_t *s = obs_get_source_by_name(context->source_audio_name);
			if(s){
				do{
					context->source_audio_filter = NULL;
					obs_source_enum_filters(s, EnumAudioFilter, data);
					if(context->source_audio_filter)
					{
						obs_source_filter_remove(s,context->source_audio_filter);
					}
				}while(context->source_audio_filter);
				obs_source_release(s);
			}
			bfree(context->source_audio_name);
			context->source_audio_name = bstrdup(source_audio_name);
		}
	}else{
		context->source_audio_name = bstrdup(source_audio_name);
	}
	const char *next_scene_name = obs_data_get_string(settings, "next_scene");
	if (context->next_scene_name){
		if(strcmp(context->next_scene_name, next_scene_name) != 0){
			bfree(context->next_scene_name);
			context->next_scene_name = bstrdup(next_scene_name);
		}
	}else{
		context->next_scene_name = bstrdup(next_scene_name);
	}

	context->duration = (long)obs_data_get_int(settings, SETTING_DURATION);
	context->visibility_action = (int)obs_data_get_int(settings, SETTING_VISIBILITY_ACTION);
	context->end_action = (int)obs_data_get_int(settings, SETTING_END_ACTION);
	context->start_delay = obs_data_get_int(settings,SETTING_START_DELAY)*1000000;

	context->speed_percent = (int)obs_data_get_int(settings, SETTING_SPEED);
	if (context->speed_percent < 1 || context->speed_percent > 200)
		context->speed_percent = 100;

	context->backward_start = obs_data_get_bool(settings, SETTING_BACKWARD);
	if(context->backward != context->backward_start)
	{
		replay_backward_hotkey(context, 0, NULL, true);
	}
	

	obs_source_t *s = obs_get_source_by_name(context->source_name);
	if(s){
		context->source_filter = NULL;
		obs_source_enum_filters(s, EnumFilter, data);
		if(!context->source_filter)
		{
			if((obs_source_get_output_flags(s) & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC)
			{
				context->source_filter = obs_source_create_private(REPLAY_FILTER_ASYNC_ID,obs_source_get_name(context->source), settings);
			}
			else
			{
				context->source_filter = obs_source_create_private(REPLAY_FILTER_ID,obs_source_get_name(context->source), settings);
			}
			if(context->source_filter){
				obs_source_filter_add(s,context->source_filter);
			}
		}else{
			obs_source_update(context->source_filter,settings);
		}

		obs_source_release(s);
	}
	s = obs_get_source_by_name(context->source_audio_name);
	if(s){
		context->source_audio_filter = NULL;
		obs_source_enum_filters(s, EnumAudioVideoFilter, data);
		if(!context->source_audio_filter)
		{
			if((obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) != 0)
			{
				context->source_audio_filter = obs_source_create_private(REPLAY_FILTER_AUDIO_ID,obs_source_get_name(context->source), settings);
			}
			if(context->source_audio_filter){
				obs_source_filter_add(s,context->source_audio_filter);
			}
		}else{
			obs_source_update(context->source_audio_filter,settings);
		}
		obs_source_release(s);
	}
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DURATION, 5);
	obs_data_set_default_int(settings, SETTING_SPEED, 100);
	obs_data_set_default_int(settings, SETTING_VISIBILITY_ACTION, VISIBILITY_ACTION_CONTINUE);
	obs_data_set_default_int(settings, SETTING_START_DELAY, 0);
	obs_data_set_default_int(settings, SETTING_END_ACTION, END_ACTION_LOOP);
	obs_data_set_default_bool(settings, SETTING_BACKWARD, false);
}

static void replay_source_show(void *data)
{
	struct replay_source *context = data;
}

static void replay_source_hide(void *data)
{
	struct replay_source *context = data;

}

static void replay_source_active(void *data)
{
	struct replay_source *context = data;
	if(context->visibility_action == VISIBILITY_ACTION_PAUSE || context->visibility_action == VISIBILITY_ACTION_CONTINUE)
	{
		if(!context->play){
			context->play = true;
			if(context->pause_timestamp)
			{
				context->start_timestamp += obs_get_video_frame_time() - context->pause_timestamp;
			}
		}
	}
	else if(context->visibility_action == VISIBILITY_ACTION_RESTART)
	{
		context->play = true;
		context->restart = true;
	}
	context->active = true;
}

static void replay_source_deactive(void *data)
{
	struct replay_source *context = data;
	if(context->visibility_action == VISIBILITY_ACTION_PAUSE)
	{
		if(context->play){
			context->play = false;
			context->pause_timestamp = obs_get_video_frame_time();
		}
	}
	else if(context->visibility_action == VISIBILITY_ACTION_RESTART)
	{
		context->play = false;
		context->restart = true;
	}
	context->active = false;
}

static void replay_restart_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(pressed){
		c->restart = true;
		c->play = true;
	}
}

static void replay_pause_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(pressed){
		if(c->play)
		{
			c->play = false;
			c->pause_timestamp = obs_get_video_frame_time();
		}else
		{
			c->play = true;
			if(c->pause_timestamp)
			{
				c->start_timestamp += obs_get_video_frame_time() - c->pause_timestamp;
				c->pause_timestamp = 0;
			}
		}
	}
}

static void replay_retrieve(struct replay_source *c)
{

	obs_source_t *s = obs_get_source_by_name(c->source_name);
	c->source_filter = NULL;
	if(s){
		obs_source_enum_filters(s, EnumFilter, c);
	}
	obs_source_t *as = obs_get_source_by_name(c->source_audio_name);
	c->source_audio_filter = NULL;
	if(as){
		obs_source_enum_filters(as, EnumAudioVideoFilter, c);
	}

	struct replay_filter* vf = c->source_filter?c->source_filter->context.data:NULL;
	struct replay_filter* af = c->source_audio_filter?c->source_audio_filter->context.data:vf;
	if(vf && vf->video_frames.size == 0)
		vf = NULL;
	if(af && af->audio_frames.size == 0)
		af = NULL;

	if(!vf && !af){
		if(s)
			obs_source_release(s);
		if(as)
			obs_source_release(as);
		return;
	}
	pthread_mutex_lock(&c->mutex);
	for(uint64_t i = 0; i < c->video_frame_count; i++)
	{
		struct obs_source_frame* frame = *(c->video_frames + i);
		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	if(c->video_frames)
		bfree(c->video_frames);
	c->video_frames = NULL;
	c->video_frame_count = 0;
	c->video_frame_position = 0;
	for(uint64_t i = 0; i < c->audio_frame_count; i++)
	{
		free_audio_packet(&c->audio_frames[i]);
	}
	if(c->audio_frames)
		bfree(c->audio_frames);
	c->audio_frames = NULL;
	c->audio_frame_position = 0;
	c->audio_frame_count = 0;
	c->start_timestamp = obs_get_video_frame_time();
	c->pause_timestamp = 0;
	c->backward = c->backward_start;
	if(c->backward){
		if(c->speed_percent == 100){
			c->trim_end = c->start_delay * -1;
		}else{
			c->trim_end = c->start_delay * c->speed_percent / -100;
		}
		c->trim_front = 0;
	}else{
		if(c->speed_percent == 100){
			c->trim_front = c->start_delay * -1;
		}else{
			c->trim_front = c->start_delay * c->speed_percent / -100;
		}
		c->trim_end = 0;
	}
	
	if(vf){
		struct obs_source_frame *frame;
		pthread_mutex_lock(&vf->mutex);
		if(vf->video_frames.size){
			circlebuf_peek_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame*));
			c->first_frame_timestamp = frame->timestamp;
			c->last_frame_timestamp = frame->timestamp;
		}
		c->video_frame_count = vf->video_frames.size/sizeof(struct obs_source_frame*);
		c->video_frames = bzalloc(c->video_frame_count * sizeof(struct obs_source_frame*));
		for(uint64_t i = 0; i < c->video_frame_count; i++)
		{
			circlebuf_pop_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame*));
			c->last_frame_timestamp = frame->timestamp;
			*(c->video_frames + i) = frame;
		}
		if(c->backward){
			c->video_frame_position = c->video_frame_count-1;
		}
		pthread_mutex_unlock(&vf->mutex);
	}
	if(af){
		struct obs_audio_info info;
		obs_get_audio_info(&info);
		pthread_mutex_lock(&af->mutex);
		struct obs_audio_data audio;
		if (!vf && af->audio_frames.size)
		{
			circlebuf_peek_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
			c->first_frame_timestamp = audio.timestamp;
			c->last_frame_timestamp = audio.timestamp;
		}
		c->audio_frame_count = af->audio_frames.size/sizeof(struct obs_audio_data);
		c->audio_frames = bzalloc(c->audio_frame_count * sizeof(struct obs_audio_data));
		for(uint64_t i = 0; i < c->audio_frame_count; i++)
		{
			circlebuf_pop_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
			if(!vf){
				c->last_frame_timestamp = audio.timestamp;
			}
			memcpy(&c->audio_frames[i], &audio, sizeof(struct obs_audio_data));
			for (size_t j = 0; j < MAX_AV_PLANES; j++) {
				if (!audio.data[j])
					break;

				c->audio_frames[i].data[j] = bmemdup(audio.data[j], c->audio_frames[i].frames * sizeof(float));
			}
		}
		pthread_mutex_unlock(&af->mutex);
	}
	pthread_mutex_unlock(&c->mutex);
	if(c->active || c->visibility_action == VISIBILITY_ACTION_CONTINUE || c->visibility_action == VISIBILITY_ACTION_NONE)
	{
		c->play = true;
	}
	if(s)
		obs_source_release(s);
	if(as)
		obs_source_release(as);
}

static void replay_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if(!pressed || !c->source_name)
		return;

	replay_retrieve(c);
}

void update_speed(struct replay_source *c, int new_speed)
{
	if(new_speed < 1)
		new_speed = 1;

	if(new_speed == c->speed_percent)
		return;
	if(c->video_frame_count)
	{
		struct obs_source_frame *peek_frame = c->video_frames[c->video_frame_position];
		uint64_t duration = peek_frame->timestamp - c->first_frame_timestamp;
		if(c->backward)
		{
			duration = c->last_frame_timestamp - peek_frame->timestamp;
		}
		const uint64_t old_duration = duration * 100 / c->speed_percent;
		const uint64_t new_duration = duration * 100 / new_speed;
		c->start_timestamp += old_duration - new_duration;
	}
	c->speed_percent = new_speed;
}

static void replay_faster_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, c->speed_percent*3/2);
}

static void replay_slower_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, c->speed_percent*2/3);
}

static void replay_normal_speed_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, 100);
}

static void replay_half_speed_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, 50);
}

static void replay_trim_front_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	const uint64_t timestamp = obs_get_video_frame_time();
	int64_t duration = timestamp - c->start_timestamp;
	if(c->backward)
	{
		duration = (c->last_frame_timestamp - c->first_frame_timestamp) - duration;
	}
	if(c->speed_percent != 100)
	{
		duration = duration * c->speed_percent / 100;
	}
	if(c->backward){
		if(c->first_frame_timestamp + c->trim_front < c->last_frame_timestamp - duration){
			c->trim_end = duration;
		}
	}else{
		if(duration + c->first_frame_timestamp < c->last_frame_timestamp - c->trim_end){
			c->trim_front = duration;
		}
	}
}

static void replay_trim_end_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;
	const uint64_t timestamp = obs_get_video_frame_time();
	if(timestamp > c->start_timestamp)
	{
		uint64_t duration = timestamp - c->start_timestamp;
		if(!c->backward)
		{
			duration = (c->last_frame_timestamp - c->first_frame_timestamp) - duration;
		}
		if(c->speed_percent != 100)
		{
			duration = duration * c->speed_percent / 100;
		}
		if(c->backward)
		{
			if(c->first_frame_timestamp + duration < c->last_frame_timestamp - c->trim_end)
			{			
				c->trim_front = duration;
			}
		}else{
			if(c->first_frame_timestamp + c->trim_front < c->last_frame_timestamp - duration)
			{			
				c->trim_end = duration;
			}
		}
	}

}

static void replay_trim_reset_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;
	c->trim_end = 0;
	if(c->speed_percent == 100){
		c->trim_front = c->start_delay * -1;
	}else{
		c->trim_front = c->start_delay * c->speed_percent / -100;
	}
}

static void *replay_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_source *context = bzalloc(sizeof(struct replay_source));
	context->source = source;
	pthread_mutex_init(&context->mutex, NULL);

	replay_source_update(context, settings);

	context->replay_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Replay",
			obs_module_text("Replay"),
			replay_hotkey, context);
	
	context->restart_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Restart",
			obs_module_text("Restart"),
			replay_restart_hotkey, context);
	
	context->pause_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Pause",
			obs_module_text("Pause"),
			replay_pause_hotkey, context);

	context->faster_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Faster",
			obs_module_text("Faster"),
			replay_faster_hotkey, context);

	context->slower_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Slower",
			obs_module_text("Slower"),
			replay_slower_hotkey, context);

	context->normal_speed_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.NormalSpeed",
			obs_module_text("Normal speed"),
			replay_normal_speed_hotkey, context);

	context->half_speed_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.HalfSpeed",
			obs_module_text("Half speed"),
			replay_half_speed_hotkey, context);

	context->trim_front_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.TrimFront",
			obs_module_text("Trim front"),
			replay_trim_front_hotkey, context);

	context->trim_end_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.TrimEnd",
			obs_module_text("Trim end"),
			replay_trim_end_hotkey, context);

	context->trim_reset_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.TrimReset",
			obs_module_text("Trim reset"),
			replay_trim_reset_hotkey, context);

	context->backward_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Backward",
			obs_module_text("Backward"),
			replay_backward_hotkey, context);

	return context;
}

static void replay_source_destroy(void *data)
{
	struct replay_source *context = data;

	if (context->source_name)
		bfree(context->source_name);

	if (context->source_audio_name)
		bfree(context->source_audio_name);

	if (context->next_scene_name)
		bfree(context->next_scene_name);

	pthread_mutex_lock(&context->mutex);

	for(uint64_t i = 0; i < context->video_frame_count; i++)
	{
		struct obs_source_frame* frame = context->video_frames[i];
		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	context->video_frame_count = 0;
	if(context->video_frames)
		bfree(context->video_frames);
	for(uint64_t i = 0; i < context->audio_frame_count; i++)
	{
		free_audio_packet(&context->audio_frames[i]);
	}
	context->audio_frame_count = 0;
	if(context->audio_frames)
		bfree(context->audio_frames);
	pthread_mutex_unlock(&context->mutex);
	pthread_mutex_destroy(&context->mutex);
	bfree(context);
}

static void replay_output_frame(struct replay_source* context, struct obs_source_frame* frame)
{
	uint64_t t = frame->timestamp;
	if(context->backward)
	{
		frame->timestamp = context->last_frame_timestamp - frame->timestamp;
	}else{
		frame->timestamp -= context->first_frame_timestamp;
	}
	if(context->speed_percent != 100)
	{
		frame->timestamp = frame->timestamp * 100 / context->speed_percent;
	}
	frame->timestamp += context->start_timestamp;
	if(context->previous_frame_timestamp <= frame->timestamp){
		context->previous_frame_timestamp = frame->timestamp;
		obs_source_output_video(context->source, frame);
	}
	frame->timestamp = t;
}

static void replay_source_tick(void *data, float seconds)
{
	struct replay_source *context = data;

	if(!context->video_frame_count && !context->audio_frame_count){
		context->play = false;
	}
	if(!context->play)
	{
		if(context->end && context->end_action == END_ACTION_HIDE)
		{
			obs_source_output_video(context->source, NULL);
		}
		return;
	}
	context->end = false;
	const uint64_t timestamp = obs_get_video_frame_time();

	pthread_mutex_lock(&context->mutex);
	if(context->video_frame_count){
		struct obs_source_frame * frame = context->video_frames[context->video_frame_position];
		if(context->backward)
		{
			if(context->video_frame_position == context->video_frame_count-1 || context->restart)
			{

				context->video_frame_position = context->video_frame_count-1;
				frame = context->video_frames[context->video_frame_position];
				context->start_timestamp = timestamp;
				context->pause_timestamp = 0;
				context->restart = false;
				if(context->trim_end != 0)
				{
					if(context->speed_percent == 100){
						context->start_timestamp -= context->trim_end;
					}else{
						context->start_timestamp -= context->trim_end * 100 / context->speed_percent;
					}
				
					if(context->trim_end < 0){
						uint64_t t = frame->timestamp;
						frame->timestamp = timestamp;
						context->previous_frame_timestamp = frame->timestamp;
						obs_source_output_video(context->source, frame);
						frame->timestamp = t;
						pthread_mutex_unlock(&context->mutex);
						return;
					}
					while(frame->timestamp > context->last_frame_timestamp - context->trim_end)
					{
						if(context->video_frame_position == 0)
						{
							context->video_frame_position = context->video_frame_count;
						}
						context->video_frame_position--;
						frame = context->video_frames[context->video_frame_position];
					}
				}
			}
			
			const int64_t video_duration = timestamp - (int64_t)context->start_timestamp;
			//TODO audio backwards
			int64_t source_duration = (context->last_frame_timestamp - frame->timestamp) * 100 / context->speed_percent;

			bool loop = false;
			struct obs_source_frame* output_frame = NULL;
			while(context->play && video_duration >= source_duration && !loop)
			{
				output_frame = frame;
				if(frame->timestamp <= context->first_frame_timestamp + context->trim_front)
				{
					if(context->end_action != END_ACTION_LOOP)
					{
						context->play = false;
						context->end = true;
					}
					else if (context->trim_end != 0)
					{
						context->restart = true;
					}
					if(context->next_scene_name && context->active)
					{
						obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
						if(s)
						{
							obs_frontend_set_current_scene(s);
							obs_source_release(s);
						}
					}
				}
				if(context->video_frame_position == 0){
					context->video_frame_position = context->video_frame_count;
					loop = true;
				}
				context->video_frame_position--;
				
				frame = context->video_frames[context->video_frame_position];

				source_duration = (context->last_frame_timestamp - frame->timestamp) * 100 / context->speed_percent;
			}
			if(output_frame){
				replay_output_frame(context, output_frame);
			}
		}else{
			if(context->video_frame_position == 0)
			{
				context->start_timestamp = timestamp;
				context->pause_timestamp = 0;
				context->restart = false;
				context->audio_frame_position = 0;
			}
			else if(context->restart)
			{
				context->video_frame_position = 0;
				context->restart = false;
				context->start_timestamp = timestamp;
				context->pause_timestamp = 0;
				context->audio_frame_position = 0;
			}
			frame = context->video_frames[context->video_frame_position];
			if(context->start_timestamp == timestamp && context->trim_front != 0){
				if(context->speed_percent == 100){
					context->start_timestamp -= context->trim_front;
				}else{
					context->start_timestamp -= context->trim_front * 100 / context->speed_percent;
				}
				if(context->trim_front < 0){
					uint64_t t = frame->timestamp;
					frame->timestamp = timestamp;
					context->previous_frame_timestamp = frame->timestamp;
					obs_source_output_video(context->source, frame);
					frame->timestamp = t;
					pthread_mutex_unlock(&context->mutex);
					return;
				}
				while(frame->timestamp < context->first_frame_timestamp + context->trim_front)
				{
					context->video_frame_position++;
					if(context->video_frame_position >= context->video_frame_count)
					{
						context->video_frame_position = 0;
					}
					frame = context->video_frames[context->video_frame_position];
				}
			}
			if(context->start_timestamp > timestamp){
				pthread_mutex_unlock(&context->mutex);
				return;
			}
			const int64_t video_duration = (int64_t)timestamp - (int64_t)context->start_timestamp;

			if(context->audio_frame_count > 1){
				struct obs_audio_data peek_audio = context->audio_frames[context->audio_frame_position];
				struct obs_audio_info info;
				obs_get_audio_info(&info);
				const int64_t frame_duration = (context->last_frame_timestamp - context->first_frame_timestamp)/context->video_frame_count;
				//const uint64_t duration = audio_frames_to_ns(info.samples_per_sec, peek_audio.frames);
				int64_t audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
				bool loop = false;
				while(context->play && video_duration + frame_duration > audio_duration && !loop)
				{
					if(peek_audio.timestamp > context->first_frame_timestamp - frame_duration && peek_audio.timestamp < context->last_frame_timestamp + frame_duration){
						context->audio.frames = peek_audio.frames;

						if(context->speed_percent != 100)
						{
							context->audio.timestamp = context->start_timestamp + (((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent);
							context->audio.samples_per_sec = info.samples_per_sec * context->speed_percent / 100;
						}else
						{
							context->audio.timestamp = peek_audio.timestamp + context->start_timestamp - context->first_frame_timestamp;
							context->audio.samples_per_sec = info.samples_per_sec;
						}
						for (size_t i = 0; i < MAX_AV_PLANES; i++) {
							context->audio.data[i] = peek_audio.data[i];
						}


						context->audio.speakers = info.speakers;
						context->audio.format = AUDIO_FORMAT_FLOAT_PLANAR;

						obs_source_output_audio(context->source, &context->audio);
					}
					context->audio_frame_position++;
					if(context->audio_frame_position >= context->audio_frame_count){
						context->audio_frame_position = 0;
						loop = true;
					}
					peek_audio = context->audio_frames[context->audio_frame_position];
					audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
				}
			}
			int64_t source_duration = (frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
			bool loop = false;
			struct obs_source_frame* output_frame = NULL;
			while(context->play && video_duration >= source_duration && !loop){
				output_frame = frame;
				if(frame->timestamp >= context->last_frame_timestamp - context->trim_end)
				{
					if(context->end_action != END_ACTION_LOOP)
					{
						context->play = false;
						context->end = true;
					}
					else if (context->trim_end != 0)
					{
						context->restart = true;
					}
					if(context->next_scene_name && context->active)
					{
						obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
						if(s)
						{
							obs_frontend_set_current_scene(s);
							obs_source_release(s);
						}
					}
				}
				context->video_frame_position++;
				if(context->video_frame_position >= context->video_frame_count)
				{
					context->video_frame_position = 0;
					loop=true;
				}
				frame = context->video_frames[context->video_frame_position];
				source_duration = (frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
			}
			if(output_frame){
				replay_output_frame(context, output_frame);
			}
		}
	}else if(context->audio_frame_count)
	{
		//no video, only audio
		struct obs_audio_data peek_audio = context->audio_frames[context->audio_frame_position];
		
		if(context->first_frame_timestamp == peek_audio.timestamp)
		{
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
			context->restart = false;
		}
		else if(context->restart)
		{
			context->audio_frame_position = 0;
			peek_audio = context->audio_frames[context->audio_frame_position];
			context->restart = false;
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
		}
		if(context->start_timestamp == timestamp && context->trim_front != 0){
			if(context->speed_percent == 100){
				context->start_timestamp -= context->trim_front;
			}else{
				context->start_timestamp -= context->trim_front * 100 / context->speed_percent;
			}
			if(context->trim_front < 0){
				pthread_mutex_unlock(&context->mutex);
				return;
			}
			bool loop = false;
			while(peek_audio.timestamp < context->first_frame_timestamp + context->trim_front && !loop)
			{
					context->audio_frame_position++;
					if(context->audio_frame_position >= context->audio_frame_count){
						context->audio_frame_position = 0;
						loop = true;
					}
					peek_audio = context->audio_frames[context->audio_frame_position];
			}
		}
		if(context->start_timestamp > timestamp){
			pthread_mutex_unlock(&context->mutex);
			return;
		}

		const int64_t video_duration = timestamp - context->start_timestamp;
		struct obs_audio_info info;
		obs_get_audio_info(&info);
		
		int64_t audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
		bool loop = false;
		while(context->play && context->audio_frame_count > 1 && video_duration >= audio_duration && !loop)
		{
			if(peek_audio.timestamp >= context->last_frame_timestamp - context->trim_end)
			{
				if(context->end_action != END_ACTION_LOOP)
				{
					context->play = false;
					context->end = true;
				}
				else if (context->trim_end != 0)
				{
					context->restart = true;
				}
				if(context->next_scene_name && context->active)
				{
					obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
					if(s)
					{
						obs_frontend_set_current_scene(s);
						obs_source_release(s);
					}
				}
			}

			context->audio.frames = peek_audio.frames;

			if(context->speed_percent != 100)
			{
				context->audio.timestamp = context->start_timestamp + (peek_audio.timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
				context->audio.samples_per_sec = info.samples_per_sec * context->speed_percent / 100;
			}else
			{
				context->audio.timestamp = peek_audio.timestamp + context->start_timestamp - context->first_frame_timestamp;
				context->audio.samples_per_sec = info.samples_per_sec;
			}
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				context->audio.data[i] = peek_audio.data[i];
			}


			context->audio.speakers = info.speakers;
			context->audio.format = AUDIO_FORMAT_FLOAT_PLANAR;

			obs_source_output_audio(context->source, &context->audio);
			context->audio_frame_position++;
			if(context->audio_frame_position >= context->audio_frame_count){
				context->audio_frame_position = 0;
				loop = true;
			}
			peek_audio = context->audio_frames[context->audio_frame_position];
			audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
		}
	}
	pthread_mutex_unlock(&context->mutex);
}
static bool EnumVideoSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if((source->info.output_flags & OBS_SOURCE_VIDEO) != 0)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}
static bool EnumAudioSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if((source->info.output_flags & OBS_SOURCE_AUDIO) != 0)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}
static bool EnumScenes(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if(source->info.type == OBS_SOURCE_TYPE_SCENE)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}
static bool replay_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct replay_source *s = data;
	replay_retrieve(s);
	return false; // no properties changed
}

static obs_properties_t *replay_source_properties(void *data)
{
	struct replay_source *s = data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t* prop = obs_properties_add_list(props,SETTING_SOURCE,TEXT_SOURCE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(EnumVideoSources, prop);
	prop = obs_properties_add_list(props,SETTING_SOURCE_AUDIO,TEXT_SOURCE_AUDIO, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(EnumAudioSources, prop);

	obs_properties_add_int(props,SETTING_DURATION,TEXT_DURATION,1,200,1);

	prop = obs_properties_add_list(props, SETTING_VISIBILITY_ACTION, "Visibility Action",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Restart", VISIBILITY_ACTION_RESTART);
	obs_property_list_add_int(prop, "Pause", VISIBILITY_ACTION_PAUSE);
	obs_property_list_add_int(prop, "Continue", VISIBILITY_ACTION_CONTINUE);
	obs_property_list_add_int(prop, "None", VISIBILITY_ACTION_NONE);

	obs_properties_add_int(props, SETTING_START_DELAY,TEXT_START_DELAY,0,100000,1000);

	prop = obs_properties_add_list(props, SETTING_END_ACTION, "End Action",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Hide", END_ACTION_HIDE);
	obs_property_list_add_int(prop, "Pause", END_ACTION_PAUSE);
	obs_property_list_add_int(prop, "Loop", END_ACTION_LOOP);

	prop = obs_properties_add_list(props,SETTING_NEXT_SCENE,TEXT_NEXT_SCENE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	//obs_enum_scenes(EnumScenes, prop);

	obs_properties_add_int_slider(props, SETTING_SPEED,
			obs_module_text("SpeedPercentage"), 1, 200, 1);
	obs_properties_add_bool(props, SETTING_BACKWARD,"Backwards");

	obs_properties_add_button(props,"replay_button","Get replay", replay_button);

	return props;
}

struct obs_source_info replay_source_info = {
	.id             = REPLAY_SOURCE_ID,
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO |
	                OBS_SOURCE_AUDIO |
	                OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = replay_source_get_name,
	.create         = replay_source_create,
	.destroy        = replay_source_destroy,
	.update         = replay_source_update,
	.get_defaults   = replay_source_defaults,
	.show           = replay_source_show,
	.hide           = replay_source_hide,
	.activate       = replay_source_active,
	.deactivate     = replay_source_deactive,
	.video_tick     = replay_source_tick,
	.get_properties = replay_source_properties
};


