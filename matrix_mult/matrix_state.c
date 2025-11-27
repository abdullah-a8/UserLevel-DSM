/**
 * @file matrix_state.c
 * @brief Matrix state management and allocation
 */

#include "matrix_mult.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int matrix_state_init(matrix_state_t *state, int N, int node_id, int num_nodes) {
    if (!state || N <= 0 || N > MAX_MATRIX_SIZE) {
        return -1;
    }

    memset(state, 0, sizeof(matrix_state_t));

    state->N = N;
    state->node_id = node_id;
    state->num_nodes = num_nodes;

    for (int i = 0; i < MAX_MATRIX_NODES; i++) {
        state->A_partitions[i] = NULL;
        state->C_partitions[i] = NULL;
    }
    state->B = NULL;
    state->my_A = NULL;
    state->my_C = NULL;

    return 0;
}

void matrix_calculate_partitions(matrix_state_t *state) {
    int N = state->N;
    int num_nodes = state->num_nodes;

    int base_rows = N / num_nodes;
    int extra_rows = N % num_nodes;

    int current_row = 0;
    for (int i = 0; i < num_nodes; i++) {
        state->rows_per_node[i] = base_rows + (i < extra_rows ? 1 : 0);
        state->start_row[i] = current_row;
        state->end_row[i] = current_row + state->rows_per_node[i];
        current_row = state->end_row[i];
    }
}

int matrix_allocate_my_partitions(matrix_state_t *state) {
    int my_id = state->node_id;
    int my_rows = state->rows_per_node[my_id];
    int N = state->N;

    size_t partition_size = my_rows * N * sizeof(double);

    state->my_A = (double*)dsm_malloc(partition_size);
    if (!state->my_A) {
        return -1;
    }

    state->my_C = (double*)dsm_malloc(partition_size);
    if (!state->my_C) {
        dsm_free(state->my_A);
        return -1;
    }

    return 0;
}

int matrix_allocate_B(matrix_state_t *state) {
    int N = state->N;
    size_t size = N * N * sizeof(double);

    state->B = (double*)dsm_malloc(size);
    if (!state->B) {
        return -1;
    }

    return 0;
}

int matrix_retrieve_all_partitions(matrix_state_t *state) {
    int num_nodes = state->num_nodes;

    for (int i = 0; i < num_nodes; i++) {
        state->A_partitions[i] = (double*)dsm_get_allocation(i * 2);
        if (!state->A_partitions[i]) {
            return -1;
        }

        state->C_partitions[i] = (double*)dsm_get_allocation(i * 2 + 1);
        if (!state->C_partitions[i]) {
            return -1;
        }
    }

    state->B = (double*)dsm_get_allocation(num_nodes * 2);
    if (!state->B) {
        return -1;
    }

    int my_id = state->node_id;
    state->my_A = state->A_partitions[my_id];
    state->my_C = state->C_partitions[my_id];

    return 0;
}

void matrix_initialize(matrix_state_t *state) {
    int my_id = state->node_id;
    int my_rows = state->rows_per_node[my_id];
    int N = state->N;

    for (int i = 0; i < my_rows; i++) {
        for (int j = 0; j < N; j++) {
            int global_row = state->start_row[my_id] + i;
            state->my_A[i * N + j] = ((global_row + j) % 10) + 1.0;
        }
    }

    if (my_id == 0) {
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) {
                state->B[i * N + j] = ((i + j) % 10) + 1.0;
            }
        }
    }

    for (int i = 0; i < my_rows; i++) {
        for (int j = 0; j < N; j++) {
            state->my_C[i * N + j] = 0.0;
        }
    }
}

void matrix_state_cleanup(matrix_state_t *state) {
    if (state->my_A) {
        dsm_free(state->my_A);
    }
    if (state->my_C) {
        dsm_free(state->my_C);
    }
    if (state->B && state->node_id == 0) {
        dsm_free(state->B);
    }
}
