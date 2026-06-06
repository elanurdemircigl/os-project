#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "ota-metadata.h"
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "sys/node-id.h"
#include "sys/log.h"

#include "firmware_data.h"
#include "crc32.c"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define SEND_INTERVAL     (10 * CLOCK_SECOND) 

#define BLOCK_SIZE 64 // Donanım kısıtları ve kararlı iletim için parça boyutu
#define OTA_END_BLOCK 0xFFFF
#define OTA_CRC_ERROR 0xFFFE

// Ortak Paket Yapısı
typedef struct {
    uint16_t block_no;          // Kaçıncı blok olduğu (Sıralama ve Durum Yönetimi için)
    uint16_t data_len;          // Paketteki gerçek firmware bayt uzunluğu 
    uint16_t checksum;          // Parça doğrulaması için basit bit kontrolü
    uint8_t data[BLOCK_SIZE];   // new-firmware.z1 dosyasından okunan ham makine kodları
} ota_packet_t;

static struct simple_udp_connection udp_conn;

// Akış ve Durum Yönetimi Değişkenleri
static uint16_t current_block = 0; 
static uint8_t ack_received = 1; // stop and wait

static uint16_t total_blocks = (FIRMWARE_PAYLOAD_LEN + BLOCK_SIZE - 1) / BLOCK_SIZE;
static uint8_t transfer_complete = 0;
static uint8_t finish_log_printed = 0;

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);
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

  //server bize sadece uint16_t tipinde bir onay numarası döndürüyor
  if(datalen == sizeof(uint16_t)) {
      uint16_t *ack_no = (uint16_t *)data;
      
      LOG_INFO("Client: Received ACK for block %" PRIu16 "\n", *ack_no);

      if(*ack_no == OTA_END_BLOCK) {
          LOG_INFO("OTA transfer completed. Final CRC accepted by server.\n");
          transfer_complete = 1;
          ack_received = 1;
          return;
      }

      if(*ack_no == OTA_CRC_ERROR) {
          LOG_ERR("Firmware CRC verification failed on server. Restarting transfer.\n");
          current_block = 0;
          ack_received = 1;
          return;
      }
      
      //gelen onay gönderdiğimiz bloğun onayı ise bir sonraki bloğa geçiş izni ver
      if(*ack_no == current_block) {
          ack_received = 1;  
          current_block++;   
      }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  static ota_packet_t packet;
  uip_ipaddr_t dest_ipaddr;
 

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);

  LOG_INFO("OTA Client Started. Total Firmware Size: %u bytes, total Blocks: %u\n", (unsigned int)FIRMWARE_PAYLOAD_LEN, total_blocks);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(transfer_complete) {
      if(!finish_log_printed) {
        LOG_INFO("OTA client stopped. No more packets will be sent.\n");
        finish_log_printed = 1;
        etimer_stop(&periodic_timer);
      }
      PROCESS_WAIT_EVENT();
      continue;
    }

    if(NETSTACK_ROUTING.node_is_reachable() &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      //sadece 2 numaralı gönderici düğüm bu bloğu çalıştırır
      if(node_id == 2) {
        
        //aktarım bitti mi?
        if (current_block >= total_blocks) {
             uint32_t crcFirmware = crc32(firmware_payload, FIRMWARE_PAYLOAD_LEN);

             packet.block_no = OTA_END_BLOCK;
             packet.data_len = sizeof(crcFirmware);
             packet.checksum = 0;
             memset(packet.data, 0, BLOCK_SIZE);
             memcpy(packet.data, &crcFirmware, sizeof(crcFirmware));

             LOG_INFO("All data blocks ACKed. Sending final CRC32: 0x%08X\n", (unsigned int)crcFirmware);
             simple_udp_sendto(&udp_conn, &packet, sizeof(ota_packet_t), &dest_ipaddr);

             etimer_set(&periodic_timer, SEND_INTERVAL);
             continue;
        }

        if(ack_received == 1) {
            packet.block_no = current_block;
            
            //bu blok için okunacak veri boyutunu hesapla
            uint16_t offset = current_block * BLOCK_SIZE;
            uint16_t remaining_bytes = FIRMWARE_PAYLOAD_LEN - offset;
            packet.data_len = (remaining_bytes < BLOCK_SIZE) ? remaining_bytes : BLOCK_SIZE;
            
            //belleği sıfırla ve gerçek firmware verisini diziye kopyala
            memset(packet.data, 0, BLOCK_SIZE);
            memcpy(packet.data, &firmware_payload[offset], packet.data_len); 
            
            //Checksum Hesaplaması
            uint16_t sum = 0;
            for(int i = 0; i < packet.data_len; i++) {
                sum += packet.data[i];
            }
            packet.checksum = sum;
            
            ack_received = 0; 
        }

        LOG_INFO("Sending OTA block %" PRIu16 "(len: %u) to ", packet.block_no, packet.data_len);
        LOG_INFO_6ADDR(&dest_ipaddr);
        LOG_INFO_("\n");

        //paketi gönderir. ACK gelmediyse zamanlayıcı dolunca aynı paketi tekrar fırlatır
        simple_udp_sendto(&udp_conn, &packet, sizeof(ota_packet_t), &dest_ipaddr);  
      }

    } else {
      LOG_INFO("Not reachable yet\n");
      
    }

    //Zaman aşımı kontrolü için periyodik tetikleme
    etimer_set(&periodic_timer, SEND_INTERVAL);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/