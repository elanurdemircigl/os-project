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

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define SEND_INTERVAL     (2 * CLOCK_SECOND) // Testlerin hızlanması için ideal süre

#define BLOCK_SIZE 64 // Donanım kısıtları ve kararlı iletim için ideal parça boyutu

// Ortak Paket Yapısı [cite: 107]
typedef struct {
    uint16_t block_no;          // Kaçıncı blok olduğu (Sıralama ve Durum Yönetimi için) [cite: 107, 118]
    uint16_t data_len;          // Paketteki gerçek firmware bayt uzunluğu 
    uint16_t checksum;          // Parça doğrulaması için basit bit kontrolü (veya CRC) [cite: 107, 119]
    uint8_t data[BLOCK_SIZE];   // new-firmware.z1 dosyasından okunan ham makine kodları [cite: 30, 73]
} ota_packet_t;

static struct simple_udp_connection udp_conn;

// Akış ve Durum Yönetimi Değişkenleri [cite: 122, 123]
static uint16_t current_block = 0; 
static uint8_t ack_received = 1; // 1: Yeni blok göndermeye hazır, 0: ACK bekliyor (Stop-and-Wait) [cite: 122]

static uint16_t total_blocks = (FIRMWARE_PAYLOAD_LEN + BLOCK_SIZE - 1) / BLOCK_SIZE;
static uint8_t transfer_complete = 0;

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
  /*
  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;
  */

  // Server bize sadece uint16_t tipinde bir ACK (onay) numarası döndürüyor
  if(datalen == sizeof(uint16_t)) {
      uint16_t *ack_no = (uint16_t *)data;
      
      LOG_INFO("Client: Received ACK for block %" PRIu16 "\n", *ack_no);
      
      // Gelen onay, gönderdiğimiz bloğun onayı ise bir sonraki bloğa geçiş izni ver [cite: 122]
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

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);

  LOG_INFO("OTA Client Started. Total Firmware Size: %u bytes, total Blocks: %u\n", (unsigned int)FIRMWARE_PAYLOAD_LEN, total_blocks);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(transfer_complete) {
      continue; //aktarım bittiyse timer ı yenilemeden döngünün sonunu bekle.  
    }

    if(NETSTACK_ROUTING.node_is_reachable() &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      // Sadece 2 numaralı gönderici düğüm bu bloğu çalıştırır [cite: 34, 38]
      if(node_id == 2) {
        
        // Aktarımın bitip bitmediğini kontrol et
        if (current_block >= total_blocks) {
             LOG_INFO("SUCCESS: All %u blocks transmitted and acknowledged!\n", total_blocks);
             transfer_complete = 1;
             continue;
        }

        if(ack_received == 1) {
            packet.block_no = current_block;
            
            // Bu blok için okunacak veri boyutunu hesapla (Son blok BLOCK_SIZE'dan küçük olabilir)
            uint16_t offset = current_block * BLOCK_SIZE;
            uint16_t remaining_bytes = FIRMWARE_PAYLOAD_LEN - offset;
            packet.data_len = (remaining_bytes < BLOCK_SIZE) ? remaining_bytes : BLOCK_SIZE;
            
            // Belleği sıfırla ve gerçek firmware verisini diziye kopyala
            memset(packet.data, 0, BLOCK_SIZE);
            memcpy(packet.data, &firmware_payload[offset], packet.data_len); 
            
            // Checksum Hesaplaması
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

        // Paketi gönderir. ACK gelmediyse zamanlayıcı dolunca aynı paketi tekrar fırlatır (Retransmission) [cite: 110, 121]
        simple_udp_sendto(&udp_conn, &packet, sizeof(ota_packet_t), &dest_ipaddr);  
      }

    } else {
      LOG_INFO("Not reachable yet\n");
      
    }

    /* Zaman aşımı (Timeout) kontrolü için periyodik tetikleme [cite: 121] */
    etimer_set(&periodic_timer, SEND_INTERVAL);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/