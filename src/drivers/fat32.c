#include "../../include/fat32.h"

// Simple alpha FAT32 driver according to the Microsoft specification
// Copyright (C) 2023 Panagiotis

#define FAT32_PARTITION_OFFSET_LBA 2048 // 1048576, 1MB

int initiateFat32() {
  printf("\n[+] FAT32: Initializing...");

  printf("\n[+] FAT32: Reading disk0 at lba %d...", FAT32_PARTITION_OFFSET_LBA);
  unsigned char rawArr[SECTOR_SIZE];
  getDiskBytes(rawArr, FAT32_PARTITION_OFFSET_LBA, 1);

  printf("\n[+] FAT32: Checking if disk0 at lba %d is FAT32 formatted...",
         FAT32_PARTITION_OFFSET_LBA);
  // get sector count
  const unsigned char littleSectorCount =
      rawArr[19] | (rawArr[20] << 8); // (two, little endian)
  int bigSectorCount = 0;
  for (int i = 3; i >= 0; i--) {
    bigSectorCount =
        (bigSectorCount << 8) | rawArr[32 + i]; // (four, little endian)
  }
  fat.sector_count =
      littleSectorCount == 0 ? bigSectorCount : littleSectorCount;

  // get FAT's
  fat.number_of_fat = rawArr[16];

  // get reserved sectors (two, little endian)
  fat.reserved_sectors = rawArr[14] | (rawArr[15] << 8);

  // get sectors / track (two, little endian)
  fat.sectors_per_track = rawArr[24] | (rawArr[25] << 8);

  // get sectors / cluster
  fat.sectors_per_cluster = rawArr[13];

  // get sectors / fat (four, little endian)
  fat.sectors_per_fat = 0;
  for (int i = 3; i >= 0; i--) {
    fat.sectors_per_fat = (fat.sectors_per_fat << 8) | rawArr[36 + i];
  }

  // volume id
  fat.volume_id = 0;
  for (int i = 0; i < 4; i++) {
    fat.volume_id = (fat.volume_id << 8) | rawArr[67 + i];
  }

  // volume name
  for (int i = 0; i < 11; i++) {
    fat.volume_label[i] = (char)rawArr[71 + i];
  }
  fat.volume_label[11] = '\0';

  if (fat.sector_count == 0 || fat.reserved_sectors == 0 ||
      fat.sectors_per_track == 0 || fat.volume_id == 0) {
    printf("\n[+] FAT32: Failed to parse FAT information... This kernel only "
           "supports FAT32!\n");
    return 0;
  }
  if (rawArr[66] != 0x28 && rawArr[66] != 0x29) {
    printf("\n[+] FAT32: Incorrect disk signature! This kernel only supports "
           "FAT32!\n");
    return 0;
  }

  fat.fat_begin_lba = FAT32_PARTITION_OFFSET_LBA + fat.reserved_sectors;
  fat.cluster_begin_lba = FAT32_PARTITION_OFFSET_LBA + fat.reserved_sectors +
                          (fat.number_of_fat * fat.sectors_per_fat);

  fat.works = 1;

  printf("\n[+] FAT32: Valid FAT32 formatted drive: [%X] %s", fat.volume_id,
         fat.volume_label);
  printf("\n    [+] Sector count: %d", fat.sector_count);
  printf("\n    [+] FAT's: %d", fat.number_of_fat);
  printf("\n    [+] Reserved sectors: %d", fat.reserved_sectors);
  printf("\n    [+] Sectors / FAT: %d", fat.sectors_per_fat);
  printf("\n    [+] Sectors / track: %d", fat.sectors_per_track);
  printf("\n    [+] Sectors / cluster: %d", fat.sectors_per_cluster);
  printf("\n");
}

int highLowCombiner(uint16_t highBits[2], uint16_t lowBits[2]) {
  uint32_t clusterNumber[2];

  int cluster[2];

  for (int i = 0; i < 2; i++) {
    cluster[i] = ((int)highBits[i]) << 16 | (int)lowBits[i];
  }

  return cluster;
}

unsigned int getFatEntry(int cluster) {
  int lba = fat.fat_begin_lba + (cluster * 4 / SECTOR_SIZE);
  int entryOffset = (cluster * 4) % SECTOR_SIZE;

  unsigned char rawArr[SECTOR_SIZE];
  getDiskBytes(rawArr, lba, 1);
  // unsigned int c = *(uint32 *)&rawArr[entryOffset] & 0x0FFFFFFF; //
  // 0x0FFFFFFF mask to keep lower 28 bits valid, nah fuck this ima do it
  // manually, watch & learn
  if (rawArr[entryOffset] >= 0xFFF8)
    return 0;

  unsigned int result = 0;
  for (int i = 3; i >= 0; i--) {
    result = (result << 8) | rawArr[entryOffset + i];
  }

  // printf("\n[%d] %x %x %x %x {Binary: %d Hexadecimal: %x}\n", entryOffset,
  //        rawArr[entryOffset], rawArr[entryOffset + 1], rawArr[entryOffset +
  //        2], rawArr[entryOffset + 3], result, result);

  return result;
}

int compFilename(string filename1, string filename2) {
  return memcmp(filename1, filename2, 11) == 0;
}

int followConventionalDirectoryLoop(string outStr, string directory,
                                    int levelDeep) {
  int currLevelDeep = -1;
  int compIng = 0;
  int len = strlength(directory);
  for (int i = 0; i < len; i++) {
    if (compIng == 0) {
      if (directory[i] == '/')
        currLevelDeep++;
      if (currLevelDeep == levelDeep)
        compIng = 1;
    } else {
      if (directory[i] == '/') {
        outStr[compIng - 1] = '\0';
        return 1;
      } else if ((i + 1) == len) {
        outStr[compIng - 1] = directory[i];
        compIng++;
        outStr[compIng - 1] = '\0';
        return 1;
      } else {
        outStr[compIng - 1] = directory[i];
        compIng++;
      }
    }
  }

  return 0;
}

char *formatToShort8_3Format(char *directory) {
  static char out[12]; // 8 characters + dot + 3 characters + null terminator
  int         i;

  for (i = 0; i < 11; i++) {
    out[i] = ' ';
  }
  out[11] = '\0';

  int len = strlength(directory);
  int dotIndex = -1;

  for (i = len - 1; i >= 0; i--) {
    if (directory[i] == '.') {
      dotIndex = i;
      break;
    }
  }

  int nameLength = (dotIndex == -1) ? len : dotIndex;
  for (i = 0; i < nameLength && i < 8; i++) {
    char c = directory[i];
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
    out[i] = c;
  }

  if (dotIndex != -1) {
    int extensionLength = len - (dotIndex + 1);
    for (i = 0; i < extensionLength && i < 3; i++) {
      char c = directory[dotIndex + 1 + i];
      if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
      }
      out[8 + i] = c;
    }
  }

  return out;
}

int charPrintNoFF(char *target) {
  if (*target != 0xFFFFFFFF)
    printf("%c", *target);
}

int calcLfn(int clusterNum, int nthOf32) {
  if (clusterNum < 2)
    return 0;

  unsigned char rawArr[SECTOR_SIZE];
  int           curr = 0;

  if (nthOf32 == 0) {
    curr = 1;
    nthOf32 = 33;
  }
  int checksum = 0x0;
  while (1) {
    const int lba = fat.cluster_begin_lba +
                    (clusterNum - 2 - curr) * fat.sectors_per_cluster;
    getDiskBytes(rawArr, lba, 1);
    for (int i = (nthOf32 - 1); i >= 0; i--) {
      if (rawArr[32 * i + 11] != 0x0F) {
        // printf("end of long \n", i);
        printf(" | checksum: 0x%x\n", checksum);
        return 1;
      }

      if (checksum == 0x0)
        printf("    [-1] ");

      for (int j = 0; j < 5; j++) {
        charPrintNoFF(&rawArr[32 * i + 1 + (j * 2)]);
      }

      for (int j = 0; j < 6; j++) {
        charPrintNoFF(&rawArr[32 * i + 14 + (j * 2)]);
      }

      for (int j = 0; j < 2; j++) {
        charPrintNoFF(&rawArr[32 * i + 28 + (j * 2)]);
      }

      checksum = rawArr[32 * i + 13];

      // if ((rawArr[32 * i] & 0x40) != 0) { // last long
      //   printf(" last long detected [index=%d] \n", i);
      // } else { // any long
      //   printf(" nth long detected [index=%d] \n", i);
      // }
    }
  }

  return 0;
}

int showCluster(int clusterNum, int attrLimitation) // NOT 0, NOT 1
{
  if (clusterNum < 2)
    return 0;

  const int lba =
      fat.cluster_begin_lba + (clusterNum - 2) * fat.sectors_per_cluster;
  unsigned char rawArr[SECTOR_SIZE];
  getDiskBytes(rawArr, lba, 1);

  for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
    if (rawArr[32 * i] == 0)
      break;

    if ((rawArr[32 * i + 11] == 0x0F) || (rawArr[32 * i + 11] == 0x08) ||
        (attrLimitation != NULL && rawArr[32 * i + 11] != attrLimitation))
      continue;

    uint8 attr = rawArr[32 * i + 11];
    uint8 reserved = rawArr[32 * i + 12];

    unsigned int createdDate = (rawArr[32 * i + 17] << 8) | rawArr[32 * i + 16];
    int          createdDay = createdDate & 0x1F;
    int          createdMonth = (createdDate >> 5) & 0xF;
    int          createdYear = ((createdDate >> 9) & 0x7F) + 1980;

    printf("[%d] attr: 0x%02X | created: %02d/%02d/%04d | ", reserved, attr,
           createdDay, createdMonth, createdYear);
    int lfn = 0;
    for (int o = 0; o < 11; o++) {
      if (rawArr[32 * i + o] == '~')
        lfn = 1;
      printf("%c", rawArr[32 * i + o]);
    }
    printf("\n");
    if (lfn)
      calcLfn(clusterNum, i);
  }

  if (rawArr[SECTOR_SIZE - 32] != 0) {
    unsigned int nextCluster = getFatEntry(clusterNum);
    if (nextCluster == 0)
      return;
    showCluster(nextCluster, attrLimitation);
  }

  return 1;
}

int showFileByCluster(int clusterNum, int size) {
  clearScreen();
  for (int i = 0; i < DIV_ROUND_CLOSEST(size, SECTOR_SIZE); i++) { // 1
    unsigned char rawArr[SECTOR_SIZE];
    const int     lba =
        fat.cluster_begin_lba + (clusterNum - 2) * fat.sectors_per_cluster + i;
    getDiskBytes(rawArr, lba, 1);
    for (int j = 0; j < SECTOR_SIZE; j++)
      printf("%c", rawArr[j]);
  }
}