#ifndef BOARD_STORE_H
#define BOARD_STORE_H
#include "board_model.h"
/* Missing user configuration falls back to the package; invalid user data is reported. */
board_result_t board_store_load(board_model_t **out, const char **message);
#endif
