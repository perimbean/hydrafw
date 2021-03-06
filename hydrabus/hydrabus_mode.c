/*
 * HydraBus/HydraNFC
 *
 * Copyright (C) 2012-2014 Benjamin VERNOUX
 * Copyright (C) 2014 Bert Vermeulen <bert@biot.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "string.h"
#include "common.h"

#include "tokenline.h"

#include "hydrabus.h"
#include "hydrabus_mode.h"
#include "mode_config.h"

#define MAYBE_CALL(x) { if (x) x(con); }
static int hydrabus_mode_write(t_hydra_console *con, t_tokenline_parsed *p,
			       int token_pos);
static int hydrabus_mode_read(t_hydra_console *con, t_tokenline_parsed *p,
			      int token_pos);

extern t_token_dict tl_dict[];
extern const mode_exec_t mode_spi_exec;
extern const mode_exec_t mode_i2c_exec;
extern const mode_exec_t mode_uart_exec;
extern const mode_exec_t mode_nfc_exec;
extern t_token tokens_mode_spi[];
extern t_token tokens_mode_i2c[];
extern t_token tokens_mode_uart[];
extern t_token tokens_mode_nfc[];

static struct {
	int token;
	t_token *tokens;
	const mode_exec_t *exec;
} modes[] = {
	{ T_SPI, tokens_mode_spi, &mode_spi_exec },
	{ T_I2C, tokens_mode_i2c, &mode_i2c_exec },
	{ T_UART, tokens_mode_uart, &mode_uart_exec },
	{ T_NFC, tokens_mode_nfc, &mode_nfc_exec },
};

const char hydrabus_mode_str_cs_enabled[] =  "/CS ENABLED\r\n";
const char hydrabus_mode_str_cs_disabled[] = "/CS DISABLED\r\n";

const char hydrabus_mode_str_read_one_u8[] = "READ: 0x%02X\r\n";
const char hydrabus_mode_str_write_one_u8[] = "WRITE: 0x%02X\r\n";
const char hydrabus_mode_str_write_read_u8[] = "WRITE: 0x%02X READ: 0x%02X\r\n";
const char hydrabus_mode_str_mul_write[] = "WRITE: ";
const char hydrabus_mode_str_mul_read[] = "READ: ";
const char hydrabus_mode_str_mul_value_u8[] = "0x%02X ";
const char hydrabus_mode_str_mul_br[] = "\r\n";

static const char mode_str_delay_us[] = "DELAY %dus\r\n";
static const char mode_str_delay_ms[] = "DELAY %dms\r\n";

static const char mode_str_write_error[] =  "WRITE error:%d\r\n";
static const char mode_str_read_error[] = "READ error:%d\r\n";
static const char mode_str_write_read_error[] = "WRITE/READ error:%d\r\n";

static void hydrabus_mode_read_error(t_hydra_console *con, uint32_t mode_status)
{
	cprintf(con, mode_str_read_error, mode_status);
}

static void hydrabus_mode_write_error(t_hydra_console *con, uint32_t mode_status)
{
	cprintf(con, mode_str_write_error, mode_status);
}

static void hydrabus_mode_write_read_error(t_hydra_console *con, uint32_t mode_status)
{
	cprintf(con, mode_str_write_read_error, mode_status);
}


void print_freq(t_hydra_console *con, uint32_t freq)
{
#define PRINT_FREQ_1GHZ	(1000000000L)
#define PRINT_FREQ_1MHZ	(1000000)
#define PRINT_FREQ_1KHZ	(1000)
	uint32_t freq_int_part;
	uint32_t freq_dec_part;
	char *suffix;

	freq_int_part = freq;
	freq_dec_part = 0;
	if (freq >= PRINT_FREQ_1GHZ) {
		freq_int_part /= PRINT_FREQ_1GHZ;
		freq_dec_part = (freq - (freq_int_part * PRINT_FREQ_1GHZ)) / (PRINT_FREQ_1MHZ*10);
		suffix = "ghz";
	} else if (freq >= PRINT_FREQ_1MHZ) {
		freq_int_part /= PRINT_FREQ_1MHZ;
		freq_dec_part = (freq - (freq_int_part * PRINT_FREQ_1MHZ)) / (PRINT_FREQ_1KHZ*10);
		suffix = "mhz";
	} else if (freq >= PRINT_FREQ_1KHZ) {
		freq_int_part /= PRINT_FREQ_1KHZ;
		freq_dec_part = (freq - (freq_int_part * PRINT_FREQ_1KHZ)) / 10;
		suffix = "khz";
	} else
		suffix = "";

	if(freq_dec_part > 0)
		cprintf(con, "%d.%02d%s", freq_int_part, freq_dec_part, suffix);
	else
		cprintf(con, "%d%s", freq_int_part, suffix);
}


int cmd_mode_init(t_hydra_console *con, t_tokenline_parsed *p)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		if (p->tokens[0] != modes[i].token)
			continue;
		con->mode->exec = modes[i].exec;
		if (con->mode->exec->init(con, p) == 0)
			return FALSE;
		if (!tl_mode_push(con->tl, modes[i].tokens))
			return FALSE;
		con->console_mode = modes[i].token;
		tl_set_prompt(con->tl, (char *)con->mode->exec->get_prompt(con));
	}

	return TRUE;
}

int cmd_mode_exec(t_hydra_console *con, t_tokenline_parsed *p)
{
	uint32_t usec;
	int t, tokens_used, factor, ret;
	bool done;

	ret = TRUE;
	done = FALSE;
	for (t = 0; !done && p->tokens[t]; t++) {
		switch (p->tokens[t]) {
		case T_CS_ON:
		case T_START:
		case T_LEFT_SQ:
			con->mode->proto.wwr = 0;
			MAYBE_CALL(con->mode->exec->start);
			break;
		case T_LEFT_CURLY:
			con->mode->proto.wwr = 1;
			MAYBE_CALL(con->mode->exec->start);
			break;
		case T_CS_OFF:
		case T_STOP:
		case T_RIGHT_SQ:
		case T_RIGHT_CURLY:
			con->mode->proto.wwr = 0;
			MAYBE_CALL(con->mode->exec->stop);
			break;
		case T_SLASH:
			MAYBE_CALL(con->mode->exec->clkh);
			break;
		case T_BACKSLASH:
			MAYBE_CALL(con->mode->exec->clkl);
			break;
		case T_MINUS:
			MAYBE_CALL(con->mode->exec->dath);
			break;
		case T_UNDERSCORE:
			MAYBE_CALL(con->mode->exec->datl);
			break;
		case T_EXCLAMATION:
			MAYBE_CALL(con->mode->exec->dats);
			break;
		case T_CARET:
			MAYBE_CALL(con->mode->exec->clk);
			break;
		case T_DOT:
			MAYBE_CALL(con->mode->exec->bitr);
			break;
		case T_AMPERSAND:
		case T_PERCENT:
			if (p->tokens[t] == T_PERCENT)
				factor = 1000;
			else
				factor = 1;
			if (p->tokens[t + 1] == T_ARG_TOKEN_SUFFIX_INT) {
				t += 2;
				memcpy(&usec, p->buf + p->tokens[t], sizeof(int));
			} else {
				usec = 1;
			}
			usec *= factor;
			DelayUs(usec);
			break;

		case T_READ:
			t += hydrabus_mode_read(con, p, t);
			break;
		case T_WRITE:
			tokens_used = hydrabus_mode_write(con, p, t + 1);
			if (!tokens_used)
				done = TRUE;
			else
				t += tokens_used;
			break;
		case T_ARG_INT:
			tokens_used = hydrabus_mode_write(con, p, t);
			if (!tokens_used)
				done = TRUE;
			else
				/*
				 * Less one because we didn't start on a "real"
				 * token, and the t++ at the end of this loop
				 * needs to take us to the next token.
				 */
				t += tokens_used - 1;
			break;
		case T_EXIT:
			MAYBE_CALL(con->mode->exec->cleanup);
			mode_exit(con, p);
			break;
		default:
			/* Mode-specific commands. */
			tokens_used = con->mode->exec->exec(con, p, t);
			if (!tokens_used)
				done = TRUE;
			else
				/*
				 * The module dispatcher considers itself to have
				 * used this token, but so does this loop. Subtract
				 * 1 to get back to it.
				 */
				t += tokens_used - 1;
		}
	}

	return ret;
}

static int chomp_integers(t_hydra_console *con, t_tokenline_parsed *p,
			  int token_pos, unsigned int *num_bytes)
{
	mode_config_proto_t* p_proto = &con->mode->proto;
	int arg_int, count, t, i;

	t = token_pos;
	*num_bytes = 0;
	while (p->tokens[t] == T_ARG_INT) {
		t++;
		memcpy(&arg_int, p->buf + p->tokens[t++], sizeof(int));
		if (arg_int > 0xff) {
			cprintf(con, "Please specify one byte at a time.\r\n");
			return 0;
		}
		p_proto->buffer_tx[(*num_bytes)++] = arg_int;

		if (p->tokens[t] == T_ARG_TOKEN_SUFFIX_INT) {
			t++;
			memcpy(&count, p->buf + p->tokens[t++], sizeof(int));
			if (*num_bytes + count > sizeof(p_proto->buffer_tx)) {
				cprintf(con, "Repeat count exceeds buffer size.\r\n");
				return 0;
			}
			/* We added one already. */
			for (i = 0; i < count - 1; i++)
				p_proto->buffer_tx[(*num_bytes)++] = arg_int;
		}
	}

	return t - token_pos;
}

/*
 * This function can be called for either T_WRITE or a free-standing
 * T_ARG_INT, so it's called with t pointing to the first token after
 * T_WRITE, or the first T_ARG_INT in the free-standing case.
 *
 * Returns the number of tokens eaten.
 */
static int hydrabus_mode_write(t_hydra_console *con, t_tokenline_parsed *p,
			       int t)
{
	mode_config_proto_t* p_proto = &con->mode->proto;
	uint32_t mode_status;
	unsigned int num_bytes;
	int count, tokens_used, i;

	tokens_used = 0;
	if (p->tokens[t] == T_ARG_TOKEN_SUFFIX_INT) {
		t++;
		memcpy(&count, p->buf + p->tokens[t++], sizeof(int));
		tokens_used += 2;
	} else {
		count = 1;
	}

	if (p->tokens[t] != T_ARG_INT) {
		cprintf(con, "No bytes to write.\r\n");
		return 0;
	}

	tokens_used += chomp_integers(con, p, t, &num_bytes);
	if (!num_bytes)
		return 0;

	for (i = 0; i < count; i++) {
		if (p_proto->wwr == 1) {
			/* Write & Read */
			mode_status = !HYDRABUS_MODE_STATUS_OK;
			if(con->mode->exec->write_read != NULL) {
				mode_status = con->mode->exec->write_read(con,
						p_proto->buffer_tx, p_proto->buffer_rx, num_bytes);
			}

			if (mode_status != HYDRABUS_MODE_STATUS_OK)
				hydrabus_mode_write_read_error(con, mode_status);
		} else {
			/* Write only */
			mode_status = !HYDRABUS_MODE_STATUS_OK;
			if(con->mode->exec->write != NULL) {
				mode_status = con->mode->exec->write(con,
								     p_proto->buffer_tx, num_bytes);
			}
			if (mode_status != HYDRABUS_MODE_STATUS_OK)
				hydrabus_mode_write_error(con, mode_status);
		}
	}

	return tokens_used;
}

/* Returns the number of tokens eaten. */
static int hydrabus_mode_read(t_hydra_console *con, t_tokenline_parsed *p,
			      int token_pos)
{
	mode_config_proto_t* p_proto;
	uint32_t mode_status;
	int count, t;

	p_proto = &con->mode->proto;

	t = token_pos;
	if (p->tokens[t + 1] == T_ARG_TOKEN_SUFFIX_INT) {
		t += 2;
		memcpy(&count, p->buf + p->tokens[t], sizeof(int));
	} else {
		count = 1;
	}

	mode_status = !HYDRABUS_MODE_STATUS_OK;
	if(con->mode->exec->read != NULL) {
		mode_status = con->mode->exec->read(con, p_proto->buffer_rx, count);
	}
	if (mode_status != HYDRABUS_MODE_STATUS_OK)
		hydrabus_mode_read_error(con, mode_status);

	return t - token_pos;
}

