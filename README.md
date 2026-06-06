# OTA Firmware Güncelleme Sistemi

## 1. Projeye Genel Bakış

Bu proje, Contiki-NG işletim sistemi üzerinde çalışan bir OTA (Over-The-Air)
firmware güncelleme sistemi implementasyonudur. Zolertia Z1 (MSP430) donanımını
ve Cooja simülatörünü hedeflemektedir.

Sistem; bir sunucu düğümü ve bir istemci düğümünden oluşur. İstemci,
`new-firmware.z1` dosyasını küçük bloklara bölerek RPL/UDP ağı üzerinden
sunucuya iletir. Sunucu gelen blokları Coffee dosya sistemine yazar ve
aktarım tamamlandığında CRC32 ile bütünlük doğrulaması yapar.

---

## 2. Dosya Yapısı

| Dosya | Açıklama |
|---|---|
| `udp-client.c` | İstemci düğümü — firmware bloklarını gönderir |
| `udp-server.c` | Sunucu düğümü — blokları alır, diske yazar, CRC doğrular |
| `firmware_data.h` | Gönderilecek firmware'in ham verisi (`firmware_payload` dizisi) |
| `crc32.c` | CRC32 hesaplama fonksiyonu ve lookup table |
| `ota-metadata.h/c` | OTA boot metadata yapısı ve slot yönetimi |
| `slot-a.ld` / `slot-b.ld` | OTA için iki ayrı bellek bölgesi linker script'i |
| `new-firmware.z1` | Aktarılacak yeni firmware binary'si |
| `rpl-udp-cooja.csc` | Cooja simülatörü için ağ konfigürasyonu |
| `Makefile` | Derleme yapılandırması |

---

## 3. Sistem Mimarisi ve Protokol Tasarımı

Sistem, IEEE 802.15.4 üzerinde çalışan RPL/IPv6/UDP protokol yığınını kullanır.

[ İstemci (Node 2) ]
|
|  UDP / IPv6 / 6LoWPAN / IEEE 802.15.4
|
[ Sunucu (Node 1) — DODAG Kökü ]

**Paket yapısı:** Her OTA paketi aşağıdaki `ota_packet_t` yapısıyla tanımlanmıştır:

```c
typedef struct {
    uint16_t block_no;   // Blok sırası
    uint16_t data_len;   // Gerçek veri uzunluğu
    uint16_t checksum;   // Basit byte toplamı
    uint8_t data[64];    // Firmware verisi
} ota_packet_t;
```

**Özel değerler:**
- `OTA_END_BLOCK (0xFFFF)` → Tüm bloklar iletildi, CRC paketi gönderiliyor
- `OTA_CRC_ERROR (0xFFFE)` → Sunucu CRC doğrulamasını reddetti

---

## 4. Ağ Topolojisi

Proje, border router olmaksızın çalışan bağımsız bir RPL ağıdır.

- **Node 1 (Sunucu):** `rpl_dag_root_start()` ile DODAG kökü olur.
  Gelen firmware bloklarını Coffee FS'e yazar.
- **Node 2 (İstemci):** `NETSTACK_ROUTING.node_is_reachable()` ile
  kök düğüme erişim sağlandığında OTA aktarımını başlatır.

MAC katmanında CSMA/CA kullanılmaktadır. TSCH tercih edilmemiştir.
Düğümler arası iletişim IEEE 802.15.4 çerçeveleriyle taşınır.

---

## 5. Aktarım Stratejisi ve Stop-and-Wait / ACK-NACK

Firmware aktarımı **Stop-and-Wait** protokolüyle gerçekleştirilir.
İstemci her bloğu gönderdikten sonra sunucudan onay bekler;
onay gelmeden bir sonraki bloğa geçmez.

**İstemci akışı:**
1. `ack_received == 1` ise yeni blok hazırla
2. Bloğu `simple_udp_sendto` ile gönder, `ack_received = 0` yap
3. `etimer` dolana kadar bekle (10 saniye)
4. ACK geldiyse `current_block++`, gelmediyse aynı bloğu tekrar gönder

**ACK mekanizması:**
- Sunucu başarılı her blok için `block_no`'yu geri gönderir → **ACK**
- Checksum hatasında sunucu yanıt vermez → istemci timeout'ta yeniden gönderir → **NACK davranışı**
- `OTA_CRC_ERROR` gelirse istemci aktarımı baştan başlatır

---

## 6. Kalıcı Depolama (Coffee File System)

Sunucu, gelen firmware bloklarını Contiki'nin Coffee File System (CFS)
katmanı aracılığıyla kalıcı belleğe yazar.

```c
fd_write = cfs_open("new_fw.bin", CFS_WRITE);
cfs_write(fd_write, received_packet->data, received_packet->data_len);
cfs_close(fd_write);
```

Tüm bloklar yazıldıktan sonra CRC doğrulaması için dosya tekrar açılır:

```c
fd_read = cfs_open("new_fw.bin", CFS_READ);
cfs_read(fd_read, file_buffer, FIRMWARE_PAYLOAD_LEN);
```

Aktarım başlamadan önce `cfs_coffee_format()` ile dosya sistemi
sıfırlanarak temiz bir başlangıç sağlanır.

---

## 7. Hata Kontrolü

Sistem üç katmanlı hata kontrolü uygular:

**Blok bazlı checksum:** Her paketin `checksum` alanı, `data` dizisinin
byte toplamıdır. Sunucu bunu yeniden hesaplar; uyuşmazlıkta paketi sessizce
reddeder ve ACK göndermez.

```c
uint16_t sum = 0;
for(int i = 0; i < packet.data_len; i++) sum += packet.data[i];
packet.checksum = sum;
```

**Sıralama kontrolü:** Sunucu yalnızca `expected_block` numaralı bloğu
kabul eder. Eski bloklar (`block_no < expected_block`) loglanarak atlanır,
sıra dışı bloklar reddedilir.

**CRC32 doğrulaması:** Tüm bloklar iletildikten sonra istemci firmware'in
CRC32 değerini hesaplayıp son paket olarak gönderir. Sunucu bu değeri
Coffee FS'teki dosyadan hesapladığı CRC32 ile karşılaştırır. Eşleşmezse
`OTA_CRC_ERROR` dönerek aktarım baştan başlatılır.

---

## 8. Durum Yönetimi

**Sunucu durum değişkenleri:**

| Değişken | Açıklama |
|---|---|
| `expected_block` | Sıradaki beklenen blok numarası |
| `fd_write` | Açık dosya tanımlayıcısı (-1 ise kapalı) |
| `transfer_complete` | Aktarım tamamlandı bayrağı |

**İstemci durum değişkenleri:**

| Değişken | Açıklama |
|---|---|
| `current_block` | Gönderilecek mevcut blok numarası |
| `ack_received` | Son bloğun onaylandı bayrağı |
| `transfer_complete` | Aktarım tamamlandı bayrağı |
| `total_blocks` | Toplam blok sayısı (derleme zamanında hesaplanır) |

---

## 9. Sistem Akışı

İstemci                                      Sunucu
|                                            |
|--- RPL ağına katıl ------------------->    |
|                                            |--- DODAG kökü ol
|<-- Ağa erişim sağlandı ---------------     |
|                                            |
|--- Block #0 (data + checksum) -------->    |
|                                            |--- Checksum doğrula
|                                            |--- CFS'e yaz
|<-- ACK #0 ----------------------------     |
|                                            |
|--- Block #1 ... Block #N ------------->    |
|<-- ACK #1 ... ACK #N -----------------     |
|                                            |
|--- END (CRC32) ----------------------->    |
|                                            |--- CRC32 doğrula
|<-- ACK END (0xFFFF) ------------------     |
|                                            |
[ Aktarım Tamamlandı ]

---

## 10. Kurulum ve Çalıştırma

**Derleme:**
```bash
make TARGET=z1
```

**Cooja simülatöründe çalıştırma:**
1. Cooja'yı aç
2. `rpl-udp-cooja.csc` dosyasını yükle
3. Simülasyonu başlat
4. Node 1 sunucu, Node 2 istemci olarak çalışır

**Renode ile çalıştırma:**
```bash
make TARGET=cc2538dk
renode-test rpl-udp.robot
```
