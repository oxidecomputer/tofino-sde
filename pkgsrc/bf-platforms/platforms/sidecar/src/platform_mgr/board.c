#ifdef __linux__
	#include <bsd/stdlib.h>
#else
	#include <stdlib.h>
#endif
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <bf_switchd/bf_switchd.h>
#include <bf_bd_cfg/bf_bd_cfg_intf.h>
#include <bf_pltfm.h>
#include <bf_bd_cfg/bf_bd_cfg_bd_map.h>
#include <bf_pltfm_bd_cfg.h>

static pltfm_bd_map_t bd_map;

typedef enum {
	CONN_CPU = 0,
	CONN_BACKPLANE = 1,
	CONN_FRONT_IO = 2,
} conn_type_t;

/*
 * The number of columns in the board map file and fields in the below port_def
 * structure.
 */
#define N_PORT_DEF_COLUMNS 14

struct port_def {
	conn_type_t conn_type;
	int connector;
	int channel;
	int mac_block;
	int mac_channel;
	int txlane;
	int txpinswap;
	int rxlane;
	int rxpinswap;
	int tx_eq_pre2;
	int tx_eq_pre1;
	int tx_eq_main;
	int tx_eq_post1;
	int tx_eq_post2;
};

struct port_line {
	struct port_def def;
	struct port_line *next;
};

static int
tokenize(char *orig_buf, int *tokens, int expected)
{
	char *buf = orig_buf;
	char *token = NULL;
	const char *delim = ",\n";
	long conv = 0;
	const char *errstr = NULL;
	const long minval = -100;
	const long maxval = 100;
	int idx = 0;

	/*
	 * Split the line on either a comma or newline.
	 *
	 * If we get back NULL in `token`, then we've reached the end of the line.
	 * If `buf` is NULL, that indicates two adjacent delimiters.
	 */
	while (((token = strsep(&buf, delim)) != NULL) && (buf != NULL)) {

		/*
		 * Attempt to convert the string to a long, checking errno and the
		 * errstr message.
		 */
		errno = 0;
		conv = strtonum(token, minval, maxval, &errstr);
		if (errno != 0) {
			fprintf(stderr, "failed to parse integer: %s\n", errstr);
			return (-1);
		}

		/*
		 * Fail if we've parsed everything we expect, as there should be no
		 * further tokens in the line.
		 */
		if (idx >= expected) {
			fprintf(stderr, "too many tokens in line\n");
			return (-1);
		}

		/*
		 * Cast the long to a int, which is safe since the min/max val are
		 * within its range.
		 */
		tokens[idx++] = (int) conv;
	}

	return (idx == expected ? 0 : -1);
}

static int
parse_line(char *buf, struct port_line **line)
{
	int tokens[N_PORT_DEF_COLUMNS];

	*line = NULL;

	/* Skip any leading whitespace */
	while (*buf == ' ' || *buf == '\t' || *buf == '\n')
		buf++;

	/* Skip over blank lines or comments */
	if ((strlen(buf) < 1) || buf[0] == '#')
		return (0);

	/* Extract the expected columns */
	if (tokenize(buf, tokens, N_PORT_DEF_COLUMNS) != 0)
		return (-1);

	*line = calloc(1, sizeof (struct port_line));
	(*line)->def.conn_type = tokens[0];
	(*line)->def.connector = tokens[1];
	(*line)->def.channel = tokens[2];
	(*line)->def.mac_block = tokens[3];
	(*line)->def.mac_channel = tokens[4];
	(*line)->def.txlane = tokens[5];
	(*line)->def.txpinswap = tokens[6];
	(*line)->def.rxlane = tokens[7];
	(*line)->def.rxpinswap = tokens[8];
	(*line)->def.tx_eq_pre2 = tokens[9];
	(*line)->def.tx_eq_pre1 = tokens[10];
	(*line)->def.tx_eq_main = tokens[11];
	(*line)->def.tx_eq_post1 = tokens[12];
	(*line)->def.tx_eq_post2 = tokens[13];
	(*line)->next = NULL;

	return (0);
}

static void
free_port_defs(struct port_line *lines)
{
	while (lines != NULL) {
		struct port_line *l = lines;
		lines = lines->next;
		free(l);
	}
}

static int
load_port_defs(FILE *f, struct port_line **lines)
{
	char *buf = NULL;
	size_t bufsz = 0;
	struct port_line *last = NULL;
	int line = 0;
	int ports = 0;

	*lines = NULL;
	while ((getline(&buf, &bufsz, f)) != -1) {
		struct port_line *l;
		if (parse_line(buf, &l) < 0) {
			printf("bad line %d: %s\n", line, buf);
			ports = -1;
			break;
		}
		if (l != NULL) {
			ports++;
			if (last != NULL) 
				last->next = l;
			else
				*lines = l;
			last = l;
		}

		line++;
	}

	free(buf);
	return (ports);
}

static serdes_lane_tx_eq_t *
alloc_serdes_lane_tx_eq_t()
{
	serdes_lane_tx_eq_t *z;

	z = malloc(sizeof (serdes_lane_tx_eq_t));
	bzero(z, sizeof(serdes_lane_tx_eq_t));
	return (z);
}

void
store_tx_eq_parameters(serdes_lane_tx_eq_t *((*prs)[2]), struct port_line *def)
{
	for (int i = 0; i < MAX_TF_SERDES_LANES_PER_CHAN; i++) {
		(*prs)[i] = alloc_serdes_lane_tx_eq_t();

		/*
		* This is an array which really appears to be indexed by the enum
		* `bf_pltfm_qsfpdd_type_t`, allowing setting different EQ
		* parameters by the type of module. We'll set all the same for now,
		* though these may need to be tuned and put into our board_map.csv
		* file at some point.
		*/
		for (int j = 0; j < MAX_QSFPDD_TYPES; j++) {
			(*prs)[i]->tx_main[j] = def->def.tx_eq_main;
			(*prs)[i]->tx_pre1[j] = def->def.tx_eq_pre1;
			(*prs)[i]->tx_pre2[j] = def->def.tx_eq_pre2;
			(*prs)[i]->tx_post1[j] = def->def.tx_eq_post1;
			(*prs)[i]->tx_post2[j] = def->def.tx_eq_post2;
		}
	}
}

int
platform_board_init(char *board_file)
{
	bd_map_ent_t *entries, *entry;
	uint8_t *connectors_seen;
	struct port_line *def, *defs;
	FILE *f;
	int rval = -1;
	int rows, connector_count;

	printf("loading board definition from %s\n", board_file);
	if ((f = fopen(board_file, "r")) == NULL) {
               perror("fopen");
	       return (-1);
	}

	connectors_seen = calloc(sizeof (uint8_t), MAX_CONNECTORS);
	if ((rows = load_port_defs(f, &defs)) < 0)
		goto done;

	entries = calloc(rows, sizeof (bd_map_ent_t));
	if (entries == NULL)
		goto done;

	def = defs;
	entry = entries;
	connector_count = 0;
	for (int row = 0; row < rows; row++) {
		entry->is_internal_port = def->def.conn_type == CONN_CPU;
		entry->connector = def->def.connector;
		entry->channel = def->def.channel;
		entry->mac_block = def->def.mac_block;
		entry->mac_ch = def->def.mac_channel;
		entry->tx_lane = def->def.txlane;
		entry->tx_pn_swap = def->def.txpinswap;
		entry->rx_lane = def->def.rxlane;
		entry->rx_pn_swap = def->def.rxpinswap;

		/*
		 * Insert the TX equalization parameters read from the file.
		 *
		 * The first element is for NRZ encodings, the latter for PAM4.
		 *
		 * NOTE: We're intentionally storing the EQ parameters, even for the CPU
		 * port, into the `tx_eq_for_qsfpdd` field. The SDE code on
		 * `pkgsrc/bf-platforms/drivers/src/bf_bd_cfg/bf_bd_cfg_intf.c:720` does
		 * not appear to switch on whether a port is internal when _getting_ the
		 * EQ parameters. It uses the DD values for all of them.
		 */
		store_tx_eq_parameters(&(entry->tx_eq_for_qsfpdd), def);
		def = def->next;
		if (entry->connector >= MAX_CONNECTORS) {
			fprintf(stderr, "Invalid connector %d seen at row %d",
			    entry->connector, row);
			rows--;
		} else {
			if (connectors_seen[entry->connector] == 0) {
				connector_count++;
				connectors_seen[entry->connector] = 1;
			}
			entry++;
		}
	}

	bd_map.bd_id = 1;
	bd_map.rows = rows;
	bd_map.bd_map = entries;
	bd_map.num_of_connectors = connector_count;
	rval = 0;

done:
	fclose(f);
	free_port_defs(defs);
	free(connectors_seen);

	return (rval);
}

pltfm_bd_map_t *
platform_pltfm_bd_map_get(int *rows) {
	if (!rows) {
		return NULL;
	}

	*rows = bd_map.rows;
	return (&bd_map);
}

int
platform_name_get_str(char *name, size_t name_size) {
	snprintf(name, name_size, "Board type: sidecar");
	name[name_size - 1] = '\0';
	return 0;
}

int
platform_num_ports_get(void)
{
	pltfm_bd_map_t *map;
	int bd_map_rows;

	if ((map = platform_pltfm_bd_map_get(&bd_map_rows)) == NULL) {
		return 0;
	} else {
		return (map->num_of_connectors);
	}
}

bf_pltfm_status_t
platform_port_mac_addr_get(bf_pltfm_port_info_t *port_info, uint8_t *mac_addr) {
	return (BF_PLTFM_OBJECT_NOT_FOUND);
}
