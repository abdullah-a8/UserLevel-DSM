/**
 * @file matrix_compute.c
 * @brief Matrix multiplication computation and verification
 */

#include "matrix_mult.h"
#include <stdio.h>
#include <math.h>

void matrix_compute(matrix_state_t *state) {
    int my_rows = state->rows_per_node[state->node_id];
    int N = state->N;

    for (int i = 0; i < my_rows; i++) {
        for (int j = 0; j < N; j++) {
            double sum = 0.0;

            for (int k = 0; k < N; k++) {
                sum += state->my_A[i * N + k] * state->B[k * N + j];
            }

            state->my_C[i * N + j] = sum;
        }
    }
}

int matrix_verify(matrix_state_t *state) {
    int my_id = state->node_id;
    int my_rows = state->rows_per_node[my_id];
    int N = state->N;

    int rows_to_check[] = {0, my_rows / 2, my_rows - 1};
    int num_checks = 3;

    if (my_rows < 3) {
        num_checks = my_rows;
        for (int i = 0; i < num_checks; i++) {
            rows_to_check[i] = i;
        }
    }

    for (int idx = 0; idx < num_checks; idx++) {
        int i = rows_to_check[idx];
        if (i >= my_rows) continue;

        int cols_to_check[] = {0, N / 2, N - 1};
        int num_col_checks = (N >= 3) ? 3 : N;

        for (int jdx = 0; jdx < num_col_checks; jdx++) {
            int j = cols_to_check[jdx];
            if (j >= N) continue;

            double expected = 0.0;
            for (int k = 0; k < N; k++) {
                expected += state->my_A[i * N + k] * state->B[k * N + j];
            }

            double actual = state->my_C[i * N + j];

            if (fabs(expected - actual) > 0.001) {
                int global_row = state->start_row[my_id] + i;
                printf("    [Node %d] Verification FAILED at C[%d][%d]: expected=%.2f, got=%.2f\n",
                       my_id, global_row, j, expected, actual);
                return 0;
            }
        }
    }

    return 1;
}

void matrix_print_sample(matrix_state_t *state, int samples) {
    int my_id = state->node_id;
    int my_rows = state->rows_per_node[my_id];
    int N = state->N;

    if (samples > my_rows) samples = my_rows;
    if (samples > 5) samples = 5;

    printf("\n[Node %d] Sample of computed result C:\n", my_id);
    printf("  (Showing first %d rows of this partition)\n\n", samples);

    printf("       ");
    int cols_to_show = (N > 10) ? 10 : N;
    for (int j = 0; j < cols_to_show; j++) {
        printf("  [%2d] ", j);
    }
    if (N > 10) printf("  ...");
    printf("\n");

    for (int i = 0; i < samples; i++) {
        int global_row = state->start_row[my_id] + i;
        printf("  [%3d]", global_row);

        for (int j = 0; j < cols_to_show; j++) {
            printf(" %5.1f", state->my_C[i * N + j]);
        }
        if (N > 10) printf("  ...");
        printf("\n");
    }

    if (my_rows > samples) {
        printf("   ...\n");
    }
    printf("\n");
}
