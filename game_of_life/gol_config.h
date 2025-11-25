/**
 * @file gol_config.h
 * @brief Game of Life configuration structures and constants
 */

#ifndef GOL_CONFIG_H
#define GOL_CONFIG_H

#include <stdint.h>
#include "dsm/dsm.h"

/* Barrier IDs - Following test_multinode.c pattern of unique barrier IDs */
#define BARRIER_ALLOC          100
#define BARRIER_INIT           101
#define BARRIER_COMPUTE_BASE   1000   /* 1000, 1001, 1002... per generation */
#define BARRIER_SWAP_BASE      2000   /* 2000, 2001, 2002... per generation */
#define BARRIER_FINAL          10000
#define BARRIER_CLEANUP        10001

/* Lock IDs */
#define LOCK_DISPLAY           5000

/* Default configuration values */
#define DEFAULT_GRID_WIDTH     100
#define DEFAULT_GRID_HEIGHT    100
#define DEFAULT_GENERATIONS    100
#define DEFAULT_DISPLAY_INTERVAL 10
#define DEFAULT_PORT           5000

/* Pattern types */
typedef enum {
    PATTERN_GLIDER,
    PATTERN_RPENTOMINO,
    PATTERN_RANDOM,
    PATTERN_BLINKER
} pattern_type_t;

/**
 * @brief Game of Life configuration
 * Matches DSM configuration pattern from test_multinode.c
 */
typedef struct {
    /* Grid parameters */
    int grid_width;          /* Grid width in cells */
    int grid_height;         /* Grid height in cells */
    int num_generations;     /* Number of generations to simulate */
    int display_interval;    /* Display every N generations (0 = no display) */

    /* Pattern initialization */
    pattern_type_t pattern;  /* Initial pattern type */
    float random_density;    /* For random pattern (0.0-1.0) */

    /* DSM parameters - Following test_multinode.c structure */
    int node_id;             /* Node ID (0 = manager) */
    int num_nodes;           /* Total nodes in cluster */
    int is_manager;          /* 1 if manager, 0 if worker */
    char manager_host[256];  /* Manager hostname (for workers) */
    int port;                /* Base port number */
    int log_level;           /* DSM log level */
} gol_config_t;

/**
 * @brief Initialize configuration with defaults
 */
static inline void gol_config_init(gol_config_t *config) {
    config->grid_width = DEFAULT_GRID_WIDTH;
    config->grid_height = DEFAULT_GRID_HEIGHT;
    config->num_generations = DEFAULT_GENERATIONS;
    config->display_interval = DEFAULT_DISPLAY_INTERVAL;
    config->pattern = PATTERN_RANDOM;
    config->random_density = 0.3f;
    config->node_id = -1;  /* Must be set */
    config->num_nodes = 2;
    config->is_manager = 0;
    config->manager_host[0] = '\0';
    config->port = DEFAULT_PORT;
    config->log_level = 3;  /* 3 = INFO level (0=NONE, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG) */
}

#endif /* GOL_CONFIG_H */
