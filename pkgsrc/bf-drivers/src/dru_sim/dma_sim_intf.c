/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/


#include <sched.h>

#ifdef __sun
#include <netinet/tcp.h>
#else
#include <linux/tcp.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>    //strlen
#include <inttypes.h>  //strlen
#include <sys/socket.h>
#include <arpa/inet.h>  //inet_addr
#include <unistd.h>     //write
#include <errno.h>

#include <dru_sim/dru_sim.h>

/* XXX: short-term fix to allow building in build on buildomat */
#ifndef TCP_QUICKACK
#define TCP_QUICKACK                    0x26
#endif

// Default TCP port base. Can be overridden by
// command-line.
//
#define TCP_PORT_BASE_DEFAULT 8001

// Offset from g_tcp_port_base
#define REG_CHNL_PORT 0
#define DMA_CHNL_PORT 1

int reg_chnl;
int dma_chnl;

int g_debug_mode = HARLYN_DEBUG_MODE;
// indicates simulation target is the Mentor emulator
int g_emu_integ = 0;
// indicates RTL-based sim is to run in parallel with tofin-model
int g_parallel_mode = 0;
char *g_model_ip_address = "127.0.0.1";

/*********************************************************************
 * create_server
 *********************************************************************/
int create_server(int listen_port /*, int local_only*/) {
  int socket_desc, client_sock, c;
  struct sockaddr_in server, client;
  int so_reuseaddr = 1;

  // Create socket
  socket_desc = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_desc == -1) {
    printf("ERROR: dru_sim could not create socket");
  }
  printf("dru_sim: listen socket created\n");

  // Prepare the sockaddr_in structure
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = htonl(INADDR_ANY);
  server.sin_port = htons(listen_port);

  setsockopt(socket_desc,
             SOL_SOCKET,
             SO_REUSEADDR,
             &so_reuseaddr,
             sizeof so_reuseaddr);

  // Bind
  if (bind(socket_desc, (struct sockaddr *)&server, sizeof(server)) < 0) {
    // print the error message
    perror("bind failed. Error");
    close(socket_desc);
    return -1;
  }
  printf("dru_sim: bind done on port %d, listening...\n", listen_port);
  // Listen
  listen(socket_desc, 1);

  // Accept an incoming connection
  puts("dru_sim: waiting for incoming connections...");
  c = sizeof(struct sockaddr_in);

  // accept connection from an incoming client
  client_sock =
      accept(socket_desc, (struct sockaddr *)&client, (socklen_t *)&c);
  if (client_sock < 0) {
    perror("accept failed");
    close(socket_desc);
    return -1;
  }
  close(socket_desc);
  printf("dru_sim: connection accepted on port %d\n", listen_port);

  return client_sock;
}

/*********************************************************************
 * create_client
 *********************************************************************/
int create_client(char *ip_addr_str, int server_port) {
  int sock;
  struct sockaddr_in server;

  // Create socket
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    printf("ERROR: dru_sim could not create socket");
    return -1;
  }
  printf("dru_sim: client socket created\n");

retry:
  server.sin_addr.s_addr = inet_addr(ip_addr_str);
  server.sin_family = AF_INET;
  server.sin_port = htons(server_port);

  // Connect to remote server
  if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
    perror("connect failed. Error");
    sleep(1);
    goto retry;
  }

  printf("dru_sim: connected on port %d\n", server_port);
  return sock;
}

/*********************************************************************
 * init_comms
 *********************************************************************/
void init_comms(int dru_mode /*0=cpu, 1=dru*/, char *model_ip, int tcp_port_base) {
  static int g_tcp_port_base = TCP_PORT_BASE_DEFAULT;

  /* Override the default TCP port base if a non-zero value is provided */
  if (tcp_port_base) {
    g_tcp_port_base = tcp_port_base;
  }
  if (model_ip) {
    g_model_ip_address = model_ip;
  }

  if (dru_mode) {
    reg_chnl = create_server(g_tcp_port_base + REG_CHNL_PORT);
    if (-1 == reg_chnl) {
      /* Indicates bind/accept error in create server */
      printf("Socket already in use. Exiting the application\n");
      exit(1);
    }
    dma_chnl =
        create_client(g_model_ip_address, g_tcp_port_base + DMA_CHNL_PORT);
    if (-1 == dma_chnl) {
      /* Indicates socket creation error in create client */
      printf("Unable to create socket. Exiting the application\n");
      exit(1);
    }
  } else {
    reg_chnl =
        create_client(g_model_ip_address, g_tcp_port_base + REG_CHNL_PORT);
    if (-1 == reg_chnl) {
      /* Indicates socket creation error in create client */
      printf("Unable to create socket. Exiting the application\n");
      exit(1);
    }
    dma_chnl = create_server(g_tcp_port_base + DMA_CHNL_PORT);
    if (-1 == dma_chnl) {
      /* Indicates bind/accept error in create server */
      printf("Socket already in use. Exiting the application\n");
      exit(1);
    }

    // make CPU side DMA channel non blocking
    // fcntl(dma_chnl, F_SETFL, O_NONBLOCK);
  }
}

/*********************************************************************
 * read_from_socket
 *********************************************************************/
int read_from_socket(int sock, uint8_t *buf, int len) {
  int n_read = 0, n_read_this_time = 0, i = 1;

  // Receive a message from client
  do {
    n_read_this_time =
        recv(sock, (buf + n_read), (len - n_read), 0 /*MSG_WAITALL*/);
    if (n_read_this_time > 0) {
      n_read += n_read_this_time;
      if (n_read < len) {
        printf("Partial recv: %d of %d so far..\n", n_read, len);
      }
    } else {
      puts("Receive failed");
      exit(3);
    }
    setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK, (void *)&i, sizeof(i));

  } while (n_read < len);
  return n_read;
}

/*********************************************************************
 * write_to_socket
 *********************************************************************/
void write_to_socket(int sock, uint8_t *buf, int len) {
  if (send(sock, buf, len, MSG_NOSIGNAL) < 0) {
    puts("Send failed");
    exit(3);
  }
}

/*********************************************************************
 * write_reg_chnl
 *********************************************************************/
static void write_reg_chnl(uint8_t *buf, int len) {
  write_to_socket(reg_chnl, buf, len);
}

/*********************************************************************
 * write_dma_chnl
 *********************************************************************/
void dma_sim_write_dma_chnl(uint8_t *buf, int len) {
  write_to_socket(dma_chnl, buf, len);
}

/*********************************************************************
 * read_reg_chnl
 *********************************************************************/
static int read_reg_chnl(uint8_t *buf, int len) {
  return read_from_socket(reg_chnl, buf, len);
}

/*********************************************************************
 * read_dma_chnl
 *********************************************************************/
int dma_sim_read_dma_chnl(uint8_t *buf, int len) {
  return read_from_socket(dma_chnl, buf, len);
}

/*********************************************************************
 * dma_sim_push_to_model
 *
 * Service a simulated PCIe register access from the LLD to/from a
 * model register.
 *********************************************************************/
uint32_t dma_sim_push_to_model(int typ,
                               uint32_t asic,
                               uint64_t reg,
                               uint64_t data) {
  (void)typ;
  (void)asic;
  (void)reg;
  (void)data;
  printf(
      "Warning: push_to_model should NOT be getting called on this target!!\n");
  return 0;
}

/********************************************************************
 * push_dma_from_dru
 *
 * This function is used when the DRU simulator is linked
 * with the model (iinstead of the LLD).
 *
 * It is called by the DRU to push a DMA request
 * to the LLD
 ********************************************************************/
int push_dma_from_dru(pcie_msg_t *msg, int len, uint8_t *buf) {
  if (g_debug_mode) {
    uint32_t *h = (uint32_t *)msg;

    printf("push_dma_from_dru: (%s) addr=%" PRIx64 ", req-len=%d dma-len=%d\n",
           (msg->typ == pcie_op_dma_rd)
               ? "Rd"
               : (msg->typ == pcie_op_dma_wr) ? "Wr" : "???",
           msg->ind_addr,
           len,
           msg->len);

    printf("Hdr: %08x %08x %08x %08x %08x %08x\n",
           *(h + 0),
           *(h + 1),
           *(h + 2),
           *(h + 3),
           *(h + 4),
           *(h + 5));
  }

  dma_sim_write_dma_chnl((unsigned char *)msg, sizeof(pcie_msg_t));

  if (msg->typ == pcie_op_dma_rd) {  // read
    int n_read = 0;

    n_read = dma_sim_read_dma_chnl(buf, msg->len);

    if (g_debug_mode) {
      unsigned int i;
      unsigned int show_len = (msg->len > 16) ? 16 : msg->len;

      printf("push_dma_from_dru: DATA-RD : n_read=%d\n", n_read);
      printf("Rd Data:");
      for (i = 0; i < show_len; i++) {
        printf(
            "%s%08x ", (i % 8) == 0 ? "\n" : "", *((uint32_t *)(&buf[4 * i])));
      }
      printf("\nPCIe DMA Rd: asic=%d: addr=%016" PRIx64 " = %08x len=%d\n",
             msg->asic,
             msg->ind_addr,
             msg->value,
             msg->len);
    }
    return (n_read);
  } else if (msg->typ == pcie_op_dma_wr) {  // write
    if (g_debug_mode) {
      unsigned int i;
      unsigned int show_len = ((msg->len / 4) > 16) ? 16 : msg->len / 4;

      printf("Wr Data:");
      for (i = 0; i < show_len; i++) {
        printf(
            "%s%08x ", (i % 8) == 0 ? "\n" : "", *((uint32_t *)(buf + 4 * i)));
      }
      printf("\n");
    }
    dma_sim_write_dma_chnl((unsigned char *)buf, msg->len);
  }
  return 0;
}

/*********************************************************************
 * write_vpi_link
 *********************************************************************/
void dma_sim_write_vpi_link(pcie_msg_t *msg) {
  write_reg_chnl((uint8_t *)msg, sizeof(pcie_msg_t));
}

/*********************************************************************
 * check_dru_ctrl_chnl
 *********************************************************************/
void *check_dru_ctrl_chnl(unsigned char *buf, int len) {
  pcie_msg_t *msg = (pcie_msg_t *)buf;

  read_reg_chnl(buf, len);

  if (g_debug_mode) {
    printf("DRU rcvd: typ=%d (%s), asic=%d: addr=%" PRIx64 " = %08x\n",
           msg->typ,
           (msg->typ == 0) ? "pcie_op_rd"
                           : (msg->typ == 1) ? "pcie_op_wr" : "unexpected",
           msg->asic,
           msg->addr,
           msg->value);
  }
  return (buf);
}

/*********************************************************************
 * check_vpi_link_cpu
 *********************************************************************/
void *check_vpi_link_cpu(unsigned char *buf, int len) {
  (void)buf;
  (void)len;
  printf(
      "Hmm, check_vpi_link_cpu shouldnt be getting called for this target\n");
  return NULL;
}

/**
 * This function is used when the DRU simulator is linked
 * with the model (iinstead of the LLD).
 *
 * It is called by the LLD to push a register read/write
 * to the model (via the DRU)
 */
unsigned int dma_sim_push_to_dru(pcie_msg_t *msg, int len) {
  write_reg_chnl((unsigned char *)msg, len);
  if (msg->typ == pcie_op_rd) {  // read

    if (g_debug_mode) {
      printf("PCIe Rd: asic=%d: addr=%" PRIx64 "..\n", msg->asic, msg->addr);
    }

    read_reg_chnl((unsigned char *)msg, len);

    if (g_debug_mode) {
      printf("PCIe Rd: asic=%d: addr=%" PRIx64 " = %08x\n",
             msg->asic,
             msg->addr,
             msg->value);
    }
    return (msg->value);
  }
  return 0;
}

/* Initialize DMA simulation interface to the model */
int dru_sim_init(char *model_ip, int tcp_port_base,
    dru_sim_dma2virt_dbg_callback_fn fn) {
#ifndef UTEST
  int ret;

  /* Initilialize DRU MTI */
  ret = dru_init_mti();
  if (ret != 0) {
    printf("ERROR: DRU sim mti init failed, ret = %d\n", ret);
    return ret;
  }

  /* Set up sockets to the model for DMA messages */
  init_comms(0, model_ip, tcp_port_base);

  /* initialize the DRU interface mode and fn pointers */
  dru_init_tcp(g_debug_mode,
               g_emu_integ,
               g_parallel_mode,
               dma_sim_write_dma_chnl,
               dma_sim_read_dma_chnl,
               dma_sim_write_vpi_link,
               dma_sim_push_to_dru,
               dma_sim_push_to_model,
               fn);

#else  /* UTEST */
  (void)tcp_port_base;
  (void)fn;
#endif /* UTEST */
  return 0;
}
