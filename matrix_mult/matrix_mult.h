/**
 * @file matrix_mult.h
 * @brief Matrix multiplication demonstration for DSM
 * 
 * Distributed matrix multiplication using row-wise partitioning.
 * Each node computes a subset of rows in the result matrix.
 */

#ifndef MATRIX_MULT_H
#define MATRIX_MULT_H

#include "dsm/dsm.h"
#include <stdint.h>

/* Configuration constants */
#define MAX_MATRIX_NODES 8
#define MAX_MATRIX_SIZE 2000

/* Barrier IDs */
#define BARRIER_ALLOC_A_BASE  100
#define BARRIER_ALLOC_B       200
#define BARRIER_ALLOC_C_BASE  300
#define BARRIER_INIT          400
#define BARRIER_COMPUTE       500
#define BARRIER_VERIFY        600
#define BARRIER_FINAL         700

/* Matrix state structure */
typedef struct {
    int N;                          /* Matrix dimension (N×N matrices) */
    int node_id;
    int num_nodes;
    
    /* Partitioning info */
    int rows_per_node[MAX_MATRIX_NODES];
    int start_row[MAX_MATRIX_NODES];
    int end_row[MAX_MATRIX_NODES];
    
    /* Matrix partitions (pointers) */
    double *A_partitions[MAX_MATRIX_NODES];  /* Row partitions of A */
    double *B;                                /* Full matrix B (shared) */
    double *C_partitions[MAX_MATRIX_NODES];  /* Row partitions of C */
    
    /* Local computation pointers */
    double *my_A;
    double *my_C;
    
} matrix_state_t;

/**
 * Initialize matrix state structure
 */
int matrix_state_init(matrix_state_t *state, int N, int node_id, int num_nodes);

/**
 * Calculate partition boundaries for each node
 */
void matrix_calculate_partitions(matrix_state_t *state);

/**
 * Allocate this node's partition of matrices A and C
 */
int matrix_allocate_my_partitions(matrix_state_t *state);

/**
 * Allocate matrix B (Node 0 only)
 */
int matrix_allocate_B(matrix_state_t *state);

/**
 * Retrieve all partitions via SVAS
 */
int matrix_retrieve_all_partitions(matrix_state_t *state);

/**
 * Initialize matrices with test values
 */
void matrix_initialize(matrix_state_t *state);

/**
 * Compute this node's portion of C = A × B
 */
void matrix_compute(matrix_state_t *state);

/**
 * Verify correctness by checking a sample of results
 */
int matrix_verify(matrix_state_t *state);

/**
 * Print a portion of matrix for debugging
 */
void matrix_print_sample(matrix_state_t *state, int samples);

/**
 * Cleanup allocated resources
 */
void matrix_state_cleanup(matrix_state_t *state);

#endif /* MATRIX_MULT_H */
