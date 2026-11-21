/*
 * INI configuration shared by all audio event recognition modules.
 */
#ifndef AER_CONFIG_H
#define AER_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define AER_DEFAULT_CONFIG	"/opt/audio-event/config.ini"
#define AER_DEFAULT_CLASS_MAP	"/opt/audio-event/yamnet_class_map.csv"

#define AER_NAME_MAX		64
#define AER_PATH_MAX		256
#define AER_MAX_WATCHED		64
#define AER_NUM_CLASSES		521

typedef struct {
	int index;			/* YAMNet class index, 0 - 520 */
	float threshold;		/* 0.0 - 1.0 */
	char name[AER_NAME_MAX];	/* name shown and sent to users */
} watched_class_t;

typedef struct {
	/* [board] */
	char board_name[AER_NAME_MAX];
	/* [audio_input] */
	char mic_name[AER_NAME_MAX];
	/* [inference] */
	char model_path[AER_PATH_MAX];
	int num_threads;
	/* [detector] */
	int cooldown_sec;
	/* [watched_classes] */
	watched_class_t watched[AER_MAX_WATCHED];
	int num_watched;
	/* [display] */
	int alert_dismiss_sec;
	char class_map_path[AER_PATH_MAX];
	/* [mqtt] */
	uint16_t broker_port;
	char topic[AER_NAME_MAX];
} app_config_t;

/*
 * Parse the INI file at path into configuration. Missing keys keep their
 * defaults. Returns 0 on success, -1 on error with a message in err.
 */
int config_parse(const char *path, app_config_t *configuration, char *err,
		 size_t errlen_max);

/* Watched class entry for a YAMNet class index, or NULL if not watched. */
const watched_class_t *config_find_watched(const app_config_t *configuration,
					   int class_index);

/*
 * Common startup: parse "-c <config>" from argv and load the configuration.
 * Exits the process on error.
 */
void config_load_from_args(int argc, char **argv, app_config_t *configuration);

/*
 * Load YAMNet display names from the class map CSV (index,mid,display_name).
 * names must have AER_NUM_CLASSES entries. Returns the number of names read.
 */
int load_class_names(const char *path, char names[][AER_NAME_MAX]);

#endif
