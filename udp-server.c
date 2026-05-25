/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"

#include "cfs/cfs.h"
#include "cfs/cfs-coffee.h"
#include "sys/node-id.h"
#include "firmware_data.h"
#include "crc32.c"

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define BLOCK_SIZE 64 // Donanım kısıtları ve kararlı iletim için ideal parça boyutu
#define OTA_END_BLOCK 0xFFFF
#define OTA_CRC_ERROR 0xFFFE

#define FIRMWARE_FILE "new_fw.bin"

typedef struct {
    uint16_t block_no;          // Kaçıncı blok olduğu (Sıralama ve Durum Yönetimi için) [cite: 107, 118]
    uint16_t data_len;          // Paketteki gerçek firmware bayt uzunluğu 
    uint16_t checksum;          // Parça doğrulaması için basit bit kontrolü (veya CRC) [cite: 107, 119]
    uint8_t data[BLOCK_SIZE];   // new-firmware.z1 dosyasından okunan ham makine kodları [cite: 30, 73]
} ota_packet_t;

static struct simple_udp_connection udp_conn;

static uint16_t expected_block = 0;
static int fd_write = -1;

static uint16_t total_blocks = (FIRMWARE_PAYLOAD_LEN + BLOCK_SIZE - 1) / BLOCK_SIZE;
static uint8_t transfer_complete = 0;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/

 static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  // 1. Gelen ham veriyi ota_packet_t yapısına dönüştür
  ota_packet_t *received_packet = (ota_packet_t *)data;

  if(transfer_complete) {
      // Final ACK kaybolursa client CRC paketini tekrar gönderebilir.
      // Bu durumda sessiz kalmak yerine final ACK'i tekrar dönüyoruz.
      if(received_packet->block_no == OTA_END_BLOCK) {
          uint16_t ack_no = OTA_END_BLOCK;
          simple_udp_sendto(&udp_conn, &ack_no, sizeof(ack_no), sender_addr);
      }
      return;
  }

  if(received_packet->block_no == OTA_END_BLOCK) {
      uint16_t ack_no;
      uint32_t received_crc;
      uint32_t calculated_crc;

      if(expected_block != total_blocks) {
          LOG_ERR("CRC paketi erken geldi! Beklenen blok: %u / %u\n", expected_block, total_blocks);
          ack_no = OTA_CRC_ERROR;
          simple_udp_sendto(&udp_conn, &ack_no, sizeof(ack_no), sender_addr);
          return;
      }

      memcpy(&received_crc, received_packet->data, sizeof(received_crc));
      calculated_crc = crc32(firmware_payload, FIRMWARE_PAYLOAD_LEN);

      if(received_crc == calculated_crc) {
          LOG_INFO("CRC32 dogrulamasi basarili! Firmware butunlugu saglandi.\n");
          LOG_INFO("================================================\n");
          LOG_INFO("OTA transfer tamamen tamamlandi.\n");
          LOG_INFO("================================================\n");
          ack_no = OTA_END_BLOCK;
          transfer_complete = 1;
      } else {
          LOG_ERR("CRC32 dogrulamasi basarisiz! Received: 0x%08X Calculated: 0x%08X\n",
                  (unsigned int)received_crc, (unsigned int)calculated_crc);
          ack_no = OTA_CRC_ERROR;
      }

      simple_udp_sendto(&udp_conn, &ack_no, sizeof(ack_no), sender_addr);
      return;
  }
  /*
  LOG_INFO("Server: Received block %u from ", received_packet->block_no);
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");
  */

  // 2. Parça Doğrulama: Gelen verinin checksum'ını hesaplayıp kontrol et
  uint16_t calculated_sum = 0;
  for(int i = 0; i < received_packet->data_len; i++) {
      calculated_sum += received_packet->data[i];
  }

  // Eğer hesaplanan değer ile paketteki değer uyuşmuyorsa paketi reddet
  if(calculated_sum != received_packet->checksum) {
      LOG_ERR("Checksum HATASI! Blok %u bozuk geldi.\n", received_packet->block_no);
      return; // Paketi işleme alma, ACK gönderme
  }

  // Sıralama ve Diske Yazma
  if(received_packet->block_no == expected_block) {
      
      if(fd_write >= 0) {
          int written = cfs_write(fd_write, received_packet->data, received_packet->data_len);
          if(written == received_packet->data_len) {
              LOG_INFO("Blok %u diske yazildi.\n", received_packet->block_no);
              expected_block++; 
              
              // BİTİŞ KONTROLÜ: Son blok da geldiyse dosyayı kapat ve doğrulama logunu bas
              if(expected_block == total_blocks) {
                  cfs_close(fd_write); // Dosya sistemini güvenli kapat
                  fd_write = -1;
                  LOG_INFO("Tum veri bloklari diske yazildi. CRC paketi bekleniyor...\n");
              }
          } else {
              LOG_ERR("Diske yazma hatasi!\n");
          }
      }
  } else if (received_packet->block_no < expected_block) {
      LOG_INFO("Eski blok %u tekrar alindi, es gecildi.\n", received_packet->block_no);
  } else {
      return;
  }
  // Onay (ACK) Mekanizması
#if WITH_SERVER_REPLY
  uint16_t ack_no = received_packet->block_no;
  simple_udp_sendto(&udp_conn, &ack_no, sizeof(ack_no), sender_addr);
#endif 
}

/*bu durunun yazdığı
  // 3. Onay (ACK) Mekanizması: Başarılı bloğun numarasını göndericiye bildir
#if WITH_SERVER_REPLY
  LOG_INFO("Sending ACK for block %u.\n", received_packet->block_no);
  uint16_t ack_no = received_packet->block_no;
  simple_udp_sendto(&udp_conn, &ack_no, sizeof(ack_no), sender_addr);
#endif 
}
*/

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  /* Initialize DAG root */
  NETSTACK_ROUTING.root_start();

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);
  
  if(node_id == 1) {
    LOG_INFO("Coffee FS formatlaniyor...\n");
    cfs_coffee_format();

    fd_write = cfs_open(FIRMWARE_FILE, CFS_WRITE);
    if(fd_write < 0) {
      LOG_ERR("Dosya olusturulamadi: %s\n", FIRMWARE_FILE);
    } else {
      LOG_INFO("Dosya hazir. Toplam %u blok bekleniyor...\n", total_blocks);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
