/**
 * @file test_display.h
 * @brief Pretty terminal display utilities for DSM tests
 *
 * Provides ANSI-styled output for visually appealing test presentations.
 * Includes box drawing, colors, progress indicators, and formatted tables.
 */

#ifndef TEST_DISPLAY_H
#define TEST_DISPLAY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

/* ════════════════════════════════════════════════════════════════════════════
 *                              ANSI COLOR CODES
 * ════════════════════════════════════════════════════════════════════════════ */

/* Reset */
#define RST     "\033[0m"

/* Regular Colors */
#define BLK     "\033[30m"
#define RED     "\033[31m"
#define GRN     "\033[32m"
#define YEL     "\033[33m"
#define BLU     "\033[34m"
#define MAG     "\033[35m"
#define CYN     "\033[36m"
#define WHT     "\033[37m"

/* Bold Colors */
#define BBLK    "\033[1;30m"
#define BRED    "\033[1;31m"
#define BGRN    "\033[1;32m"
#define BYEL    "\033[1;33m"
#define BBLU    "\033[1;34m"
#define BMAG    "\033[1;35m"
#define BCYN    "\033[1;36m"
#define BWHT    "\033[1;37m"

/* Dim Colors */
#define DIM     "\033[2m"
#define DGRY    "\033[2;37m"

/* Background Colors */
#define BG_BLK  "\033[40m"
#define BG_RED  "\033[41m"
#define BG_GRN  "\033[42m"
#define BG_YEL  "\033[43m"
#define BG_BLU  "\033[44m"
#define BG_MAG  "\033[45m"
#define BG_CYN  "\033[46m"
#define BG_WHT  "\033[47m"

/* Special */
#define BOLD    "\033[1m"
#define ULINE   "\033[4m"
#define BLINK   "\033[5m"
#define INVERT  "\033[7m"

/* Cursor Control */
#define CLEAR_LINE  "\033[2K"
#define CURSOR_UP   "\033[1A"
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"

/* ════════════════════════════════════════════════════════════════════════════
 *                              BOX DRAWING CHARS
 * ════════════════════════════════════════════════════════════════════════════ */

/* Double Line Box */
#define BOX_TL      "╔"
#define BOX_TR      "╗"
#define BOX_BL      "╚"
#define BOX_BR      "╝"
#define BOX_H       "═"
#define BOX_V       "║"
#define BOX_LT      "╠"
#define BOX_RT      "╣"
#define BOX_TT      "╦"
#define BOX_BT      "╩"
#define BOX_X       "╬"

/* Single Line Box */
#define SBOX_TL     "┌"
#define SBOX_TR     "┐"
#define SBOX_BL     "└"
#define SBOX_BR     "┘"
#define SBOX_H      "─"
#define SBOX_V      "│"
#define SBOX_LT     "├"
#define SBOX_RT     "┤"

/* Rounded Box */
#define RBOX_TL     "╭"
#define RBOX_TR     "╮"
#define RBOX_BL     "╰"
#define RBOX_BR     "╯"

/* Block Elements */
#define BLOCK_FULL  "█"
#define BLOCK_DARK  "▓"
#define BLOCK_MED   "▒"
#define BLOCK_LIGHT "░"

/* Arrows */
#define ARROW_R     "→"
#define ARROW_L     "←"
#define ARROW_U     "↑"
#define ARROW_D     "↓"
#define ARROW_LR    "↔"
#define ARROW_UD    "↕"
#define ARROW_BR    "➜"

/* Status Icons */
#define ICON_CHECK  "✓"
#define ICON_CROSS  "✗"
#define ICON_WARN   "⚠"
#define ICON_INFO   "ℹ"
#define ICON_STAR   "★"
#define ICON_DOT    "●"
#define ICON_CIRCLE "○"
#define ICON_SQUARE "■"
#define ICON_DIAMOND "◆"
#define ICON_GEAR   "⚙"
#define ICON_BOLT   "⚡"
#define ICON_CLOCK  "⏱"
#define ICON_NET    "⇄"
#define ICON_MEM    "▤"
#define ICON_LOCK   "🔒"
#define ICON_UNLOCK "🔓"

/* ════════════════════════════════════════════════════════════════════════════
 *                              UTILITY FUNCTIONS
 * ════════════════════════════════════════════════════════════════════════════ */

/* Terminal width detection */
static inline int get_term_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_col > 0 ? w.ws_col : 80;
    }
    return 80;
}

/* Print repeated character */
static inline void print_repeat(const char *str, int count) {
    for (int i = 0; i < count; i++) {
        printf("%s", str);
    }
}

/* Print centered text */
static inline void print_centered(const char *text, int width) {
    int len = strlen(text);
    int pad = (width - len) / 2;
    if (pad > 0) print_repeat(" ", pad);
    printf("%s", text);
    if (pad > 0) print_repeat(" ", width - len - pad);
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              DISPLAY FUNCTIONS
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Print a beautiful banner/header
 */
static inline void display_banner(const char *title, const char *subtitle) {
    int width = 60;
    
    printf("\n");
    printf(BCYN);
    printf("  %s", BOX_TL);
    print_repeat(BOX_H, width - 4);
    printf("%s\n", BOX_TR);
    
    printf("  %s", BOX_V);
    printf(BWHT);
    print_centered(title, width - 4);
    printf(BCYN "%s\n", BOX_V);
    
    if (subtitle && strlen(subtitle) > 0) {
        printf("  %s", BOX_V);
        printf(DGRY);
        print_centered(subtitle, width - 4);
        printf(BCYN "%s\n", BOX_V);
    }
    
    printf("  %s", BOX_BL);
    print_repeat(BOX_H, width - 4);
    printf("%s\n", BOX_BR);
    printf(RST "\n");
}

/**
 * Print a section header
 */
static inline void display_section(const char *title) {
    printf("\n");
    printf(BYEL "  ┌─");
    print_repeat("─", strlen(title) + 2);
    printf("─┐\n");
    printf("  │ %s%s%s │\n", BWHT, title, BYEL);
    printf("  └─");
    print_repeat("─", strlen(title) + 2);
    printf("─┘" RST "\n\n");
}

/**
 * Print a test start indicator
 */
static inline void display_test_start(const char *test_name, const char *description) {
    printf(BCYN "  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │ " BWHT "%s TEST" BCYN "                                       \n", ICON_GEAR);
    printf("  │ " BYEL "%-49s" BCYN " │\n", test_name);
    printf("  │ " DIM "%-49s" BCYN " │\n", description);
    printf("  └─────────────────────────────────────────────────────┘" RST "\n");
}

/**
 * Print a test phase/step
 */
static inline void display_step(int node_id, const char *action) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    printf("    %s[Node %d]%s %s\n", color, node_id, RST, action);
}

/**
 * Print an action with an icon
 */
static inline void display_action(int node_id, const char *icon, const char *action) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    printf("    %s%s%s %s[Node %d]%s %s\n", BYEL, icon, RST, color, node_id, RST, action);
}

/**
 * Print memory operation
 */
static inline void display_memory_op(int node_id, const char *op, void *addr, int value) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    
    if (strcmp(op, "WRITE") == 0) {
        printf("    %s%s%s %s[Node %d]%s WRITE " DIM "→" RST " addr=%p value=" BWHT "%d" RST "\n",
               BRED, ICON_MEM, RST, color, node_id, RST, addr, value);
    } else {
        printf("    %s%s%s %s[Node %d]%s READ  " DIM "←" RST " addr=%p value=" BWHT "%d" RST "\n",
               BGRN, ICON_MEM, RST, color, node_id, RST, addr, value);
    }
}

/**
 * Print page migration event
 */
static inline void display_page_transfer(int from_node, int to_node, const char *reason) {
    const char *from_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *to_colors[] = {BGRN, BMAG, BCYN, BYEL};
    
    printf("    %s%s%s Page Transfer: %s[Node %d]%s %s─%s%s─%s %s[Node %d]%s %s(%s)%s\n",
           BCYN, ICON_NET, RST,
           from_colors[from_node % 4], from_node, RST,
           BYEL, ARROW_R, ARROW_R, RST,
           to_colors[to_node % 4], to_node, RST,
           DIM, reason, RST);
}

/**
 * Print barrier synchronization
 */
static inline void display_barrier(int barrier_id, int num_nodes) {
    printf("    %s%s%s Barrier #%d: All %d nodes synchronized\n",
           BYEL, ICON_CLOCK, RST, barrier_id, num_nodes);
}

/**
 * Print lock operation
 */
static inline void display_lock_op(int node_id, const char *op, int lock_id) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    
    if (strcmp(op, "ACQUIRE") == 0) {
        printf("    %s%s%s %s[Node %d]%s Lock #%d " BGRN "acquired" RST "\n",
               BYEL, ICON_LOCK, RST, color, node_id, RST, lock_id);
    } else {
        printf("    %s%s%s %s[Node %d]%s Lock #%d " DGRY "released" RST "\n",
               DGRY, ICON_UNLOCK, RST, color, node_id, RST, lock_id);
    }
}

/**
 * Print test result (pass/fail)
 */
static inline void display_test_result(const char *test_name, int passed) {
    if (passed) {
        printf("\n" BGRN "  ╔═══════════════════════════════════════════════════════╗\n");
        printf("  ║   %s  TEST PASSED: %-35s ║\n", ICON_CHECK, test_name);
        printf("  ╚═══════════════════════════════════════════════════════╝" RST "\n\n");
    } else {
        printf("\n" BRED "  ╔═══════════════════════════════════════════════════════╗\n");
        printf("  ║   %s  TEST FAILED: %-35s ║\n", ICON_CROSS, test_name);
        printf("  ╚═══════════════════════════════════════════════════════╝" RST "\n\n");
    }
}

/**
 * Print verification step
 */
static inline void display_verify(int node_id, int expected, int actual, int passed) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    
    if (passed) {
        printf("    %s%s%s %s[Node %d]%s Verify: expected=%d, got=%d " BGRN "%s" RST "\n",
               BGRN, ICON_CHECK, RST, color, node_id, RST, expected, actual, ICON_CHECK);
    } else {
        printf("    %s%s%s %s[Node %d]%s Verify: expected=%d, got=%d " BRED "%s" RST "\n",
               BRED, ICON_CROSS, RST, color, node_id, RST, expected, actual, ICON_CROSS);
    }
}

/**
 * Print a divider line
 */
static inline void display_divider(void) {
    printf(DIM "    ");
    print_repeat("─", 50);
    printf(RST "\n");
}

/**
 * Print node info box
 */
static inline void display_node_info(int node_id, int is_manager, int num_nodes, const char *host) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    const char *role = is_manager ? "MANAGER" : "WORKER";
    
    printf("\n");
    printf("%s  ╔═══════════════════════════════════════╗\n", color);
    printf("  ║  " BWHT "Node Configuration" "%s                  ║\n", color);
    printf("  ╠═══════════════════════════════════════╣\n");
    printf("  ║  " RST "Node ID    : " BWHT "%-3d" "%s                    ║\n", node_id, color);
    printf("  ║  " RST "Role       : " BWHT "%-8s" "%s               ║\n", role, color);
    printf("  ║  " RST "Cluster    : " BWHT "%d nodes" "%s                  ║\n", num_nodes, color);
    if (!is_manager && host) {
        printf("  ║  " RST "Manager    : " BWHT "%-20s" "%s  ║\n", host, color);
    }
    printf("  ╚═══════════════════════════════════════╝" RST "\n\n");
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              STATISTICS DISPLAY
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Display DSM statistics in a beautiful table
 */
static inline void display_stats_header(int node_id) {
    const char *node_colors[] = {BGRN, BMAG, BCYN, BYEL};
    const char *color = node_colors[node_id % 4];
    
    printf("\n");
    printf("%s  ╔═══════════════════════════════════════════════════════════╗\n", color);
    printf("  ║" BWHT "            DSM PERFORMANCE STATISTICS" "%s                   ║\n", color);
    printf("  ║" DGRY "                      Node %-2d" "%s                           ║\n", node_id, color);
    printf("  ╠═══════════════════════════════════════════════════════════╣" RST "\n");
}

static inline void display_stats_section(const char *title) {
    printf(BYEL "  ║  " BWHT "%-53s" BYEL "    ║" RST "\n", title);
    printf(BYEL "  ╟───────────────────────────────────────────────────────────╢" RST "\n");
}

static inline void display_stat_row(const char *label, uint64_t value, const char *unit) {
    /* Fixed width: 59 chars inside the box */
    /* "  " + label(30) + " : " + value(10) + " " + unit(8) + "  " = 55 + 4 spacing = 59 */
    char line[128];
    if (unit && strlen(unit) > 0) {
        snprintf(line, sizeof(line), "  %-28s : %10lu %-8s", label, value, unit);
    } else {
        snprintf(line, sizeof(line), "  %-28s : %10lu         ", label, value);
    }
    printf(BYEL "  ║" RST "  %-55s" BYEL "║" RST "\n", line);
}

static inline void display_stat_row_str(const char *label, const char *value) {
    printf(BYEL "  ║  " RST "  %-30s : " BWHT "%-18s" RST " " BYEL "  ║" RST "\n", label, value);
}

static inline void display_stats_footer(void) {
    printf(BYEL "  ╚═══════════════════════════════════════════════════════════╝" RST "\n\n");
}

/**
 * Complete stats display from dsm_stats_t
 */
static inline void display_full_stats(int node_id, uint64_t page_faults, uint64_t read_faults,
                                       uint64_t write_faults, uint64_t pages_fetched,
                                       uint64_t pages_sent, uint64_t inv_sent, uint64_t inv_recv,
                                       uint64_t bytes_sent, uint64_t bytes_recv,
                                       uint64_t avg_latency_us, uint64_t max_latency_us,
                                       uint64_t min_latency_us) {
    display_stats_header(node_id);
    
    display_stats_section("Page Faults");
    display_stat_row("Total Page Faults", page_faults, "");
    display_stat_row("Read Faults", read_faults, "");
    display_stat_row("Write Faults", write_faults, "");
    
    printf(BYEL "  ╟───────────────────────────────────────────────────────────╢" RST "\n");
    display_stats_section("Page Transfers");
    display_stat_row("Pages Fetched (incoming)", pages_fetched, "pages");
    display_stat_row("Pages Sent (outgoing)", pages_sent, "pages");
    display_stat_row("Invalidations Sent", inv_sent, "");
    display_stat_row("Invalidations Received", inv_recv, "");
    
    printf(BYEL "  ╟───────────────────────────────────────────────────────────╢" RST "\n");
    display_stats_section("Network I/O");
    display_stat_row("Bytes Sent", bytes_sent, "bytes");
    display_stat_row("Bytes Received", bytes_recv, "bytes");
    
    printf(BYEL "  ╟───────────────────────────────────────────────────────────╢" RST "\n");
    display_stats_section("Latency Metrics");
    display_stat_row("Avg Fault Latency", avg_latency_us, "us");
    display_stat_row("Max Fault Latency", max_latency_us, "us");
    display_stat_row("Min Fault Latency", min_latency_us, "us");
    
    display_stats_footer();
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              TEST SUMMARY
 * ════════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    int passed;
} test_result_t;

/**
 * Display final test summary
 */
static inline void display_test_summary(test_result_t *results, int num_tests, int node_id) {
    int passed = 0;
    int failed = 0;
    
    for (int i = 0; i < num_tests; i++) {
        if (results[i].passed) passed++;
        else failed++;
    }
    
    printf("\n");
    printf(BCYN "  ╔═══════════════════════════════════════════════════════════╗\n");
    printf("  ║" BWHT "                      TEST SUMMARY" BCYN "                        ║\n");
    printf("  ║" DGRY "                        Node %-2d" BCYN "                          ║\n", node_id);
    printf("  ╠═══════════════════════════════════════════════════════════╣\n");
    printf("  ║                                                           ║\n");
    
    for (int i = 0; i < num_tests; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%-50s", results[i].name);
        if (results[i].passed) {
            printf("  ║   " BGRN "%s" BCYN "  %s  ║\n", ICON_CHECK, line);
        } else {
            printf("  ║   " BRED "%s" BCYN "  %s  ║\n", ICON_CROSS, line);
        }
    }
    
    printf("  ║                                                           ║\n");
    printf("  ╠═══════════════════════════════════════════════════════════╣\n");
    
    if (failed == 0) {
        printf("  ║" BGRN "             All %d tests passed!  " BCYN "                        ║\n", passed);
    } else {
        printf("  ║   " BYEL "Passed: %d" BCYN "  |  " BRED "Failed: %d" BCYN "                                   ║\n", passed, failed);
    }
    
    printf("  ╚═══════════════════════════════════════════════════════════╝" RST "\n\n");
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              PROGRESS INDICATOR
 * ════════════════════════════════════════════════════════════════════════════ */

static const char *spinner_frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
static int spinner_frame = 0;

static inline void display_spinner(const char *message) {
    printf("\r    %s%s%s %s", BCYN, spinner_frames[spinner_frame % 10], RST, message);
    fflush(stdout);
    spinner_frame++;
}

static inline void display_spinner_done(const char *message) {
    printf("\r" CLEAR_LINE "    %s%s%s %s\n", BGRN, ICON_CHECK, RST, message);
}

/**
 * Simple progress bar
 */
static inline void display_progress(const char *label, int current, int total) {
    int bar_width = 30;
    int filled = (current * bar_width) / total;
    int percent = (current * 100) / total;
    
    printf("\r    %-20s [", label);
    printf(BGRN);
    print_repeat(BLOCK_FULL, filled);
    printf(DGRY);
    print_repeat(BLOCK_LIGHT, bar_width - filled);
    printf(RST "] %3d%%", percent);
    fflush(stdout);
    
    if (current == total) {
        printf("\n");
    }
}

#endif /* TEST_DISPLAY_H */
