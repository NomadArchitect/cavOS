#include <disk.h>
#include <fs_controller.h>
#include <linked_list.h>
#include <malloc.h>
#include <string.h>
#include <system.h>
#include <task.h>
#include <util.h>

#include "./fatfs/ff.h"

bool fsUnmount(MountPoint *mnt) {
  LinkedListUnregister(&firstMountPoint, mnt);

  switch (mnt->filesystem) {
  case FS_FAT32:
    // todo!!
    f_unmount("/");
    break;
  }

  free(mnt->prefix);
  free(mnt);

  return true;
}

bool isFat(mbr_partition *mbr) {
  uint8_t *rawArr = (uint8_t *)malloc(SECTOR_SIZE);
  getDiskBytes(rawArr, mbr->lba_first_sector, 1);

  bool ret = (rawArr[66] == 0x28 || rawArr[66] == 0x29);

  free(rawArr);

  return ret;
}

// prefix MUST end with '/': /mnt/handle/
MountPoint *fsMount(char *prefix, CONNECTOR connector, uint32_t disk,
                    uint8_t partition) {
  MountPoint *mount =
      (MountPoint *)LinkedListAllocate(&firstMountPoint, sizeof(MountPoint));

  uint32_t strlen = strlength(prefix);
  mount->prefix = (char *)(malloc(strlen + 1));
  memcpy(mount->prefix, prefix, strlen);
  mount->prefix[strlen] = '\0'; // null terminate

  mount->disk = disk;
  mount->partition = partition;
  mount->connector = connector;

  bool ret = false;
  switch (connector) {
  case CONNECTOR_AHCI:
    if (!openDisk(disk, partition, &mount->mbr)) {
      fsUnmount(mount);
      return 0;
    }

    if (isFat(&mount->mbr)) {
      mount->filesystem = FS_FAT32;
      mount->fsInfo = malloc(sizeof(FATFS));
      memset(mount->fsInfo, 0, sizeof(FATFS));
      ret = f_mount(mount->fsInfo, "/", 1) ==
            FR_OK; // todo: (maybe) multiple drives
    }
    break;
  case CONNECTOR_DUMMY:
    mount->filesystem = FS_TEST;
    ret = 1;
    break;
  }

  if (!ret) {
    fsUnmount(mount);
    return 0;
  }

  if (!systemDiskInit && strlength(prefix) == 1 && prefix[0] == '/')
    systemDiskInit = true;
  return mount;
}

MountPoint *fsDetermineMountPoint(char *filename) {
  MountPoint *largestAddr = 0;
  uint32_t    largestLen = 0;

  MountPoint *browse = firstMountPoint;
  while (browse) {
    if (strlength(browse->prefix) > largestLen &&
        memcmp(filename, browse->prefix, strlength(browse->prefix)) == 0) {
      largestAddr = browse;
      largestLen = strlength(browse->prefix);
    }
    browse = browse->next;
  }

  return largestAddr;
}

OpenFile *fsKernelRegisterNode() {
  return LinkedListAllocate(&firstKernelFile, sizeof(OpenFile));
}

bool fsKernelUnregisterNode(OpenFile *file) {
  return LinkedListUnregister(&firstKernelFile, file);
}

OpenFile *fsUserRegisterNode(Task *task) {
  return LinkedListAllocate(&task->firstFile, sizeof(OpenFile));
}

bool fsUserUnregisterNode(Task *task, OpenFile *file) {
  return LinkedListUnregister(&task->firstFile, file);
}

OpenFile *fsUserNodeFetch(Task *task, int fd) {
  OpenFile *browse = task->firstFile;
  while (browse) {
    if (browse->id == fd)
      break;
    browse = browse->next;
  }
  return browse;
}

bool fsCloseFsSpecific(OpenFile *file) {
  bool res = false;
  switch (file->mountPoint->filesystem) {
  case FS_FAT32:
    f_close(file->dir);
    free(file->dir);
    res = true;
    break;
  }

  return res;
}

bool fsOpenFsSpecific(char *filename, MountPoint *mnt, OpenFile *target) {
  bool  res = false;
  char *strippedFilename = (char *)((size_t)filename + strlength(mnt->prefix) -
                                    1); // -1 for putting start slash
  switch (mnt->filesystem) {
  case FS_FAT32:
    target->dir = malloc(sizeof(FIL));
    memset(target->dir, 0, sizeof(FIL));
    res = f_open(target->dir, filename, target->mode) == FR_OK;
    break;
  case FS_TEST:
    res = 1;
    break;
  }
  return res;
}

// todo: sanitize filenames
int       openId = 2;
OpenFile *fsOpenGeneric(char *filename, Task *task, uint16_t mode) {
  OpenFile *target = task ? fsUserRegisterNode(task) : fsKernelRegisterNode();
  target->id = openId++;
  target->mode = mode;

  target->pointer = 0;
  target->tmp1 = 0;

  MountPoint *mnt = fsDetermineMountPoint(filename);
  if (!mnt) {
    // no mountpoint for this
    fsKernelUnregisterNode(target);
    free(target);
    return 0;
  }
  target->mountPoint = mnt;

  bool res = fsOpenFsSpecific(filename, mnt, target);

  if (!res) {
    // failed to open
    if (task)
      fsUserUnregisterNode(task, target);
    else
      fsKernelUnregisterNode(target);
    free(target);
    return 0;
  }

  return target;
}

OpenFile *fsKernelOpen(char *filename, uint16_t mode) {
  return fsOpenGeneric(filename, 0, mode);
}

int fsUserOpen(char *filename, int flags, uint16_t mode) {
  // todo: modes & flags
  OpenFile *file = fsOpenGeneric(filename, currentTask, mode);
  if (!file)
    return -1;

  return file->id;
}

bool fsCloseGeneric(OpenFile *file, Task *task) {
  if (task)
    fsUserUnregisterNode(task, file);
  else
    fsKernelUnregisterNode(file);

  bool res = fsCloseFsSpecific(file);

  free(file);
  return res;
}

bool fsKernelClose(OpenFile *file) { return fsCloseGeneric(file, 0); }

int fsUserClose(int fd) {
  if (fd < 2)
    return -1;
  OpenFile *file = fsUserNodeFetch(currentTask, fd);
  if (!file)
    return -1;
  bool res = fsCloseGeneric(file, currentTask);
  if (res)
    return 1;
  else
    return -1;
}

uint32_t fsGetFilesize(OpenFile *file) {
  switch (file->mountPoint->filesystem) {
  case FS_FAT32:
    return f_size((FIL *)file->dir);
    break;
  case FS_TEST:
    return 4096;
    break;
  }

  return 0;
}

uint32_t fsRead(OpenFile *file, char *out, uint32_t limit) {
  uint32_t ret = 0;
  switch (file->mountPoint->filesystem) {
  case FS_FAT32:
    unsigned int read = 0;
    bool         output = f_read(file->dir, out, limit, &read) == FR_OK;
    if (output)
      ret = read;
    break;
  case FS_TEST:
    memset(out, 'p', limit);
    break;
  }
  return ret;
}

uint32_t fsWrite(OpenFile *file, char *in, uint32_t limit) {
  uint32_t ret = 0;
  switch (file->mountPoint->filesystem) {
  case FS_FAT32:
    unsigned int write = 0;
    bool         output = f_write(file->dir, in, limit, &write) == FR_OK;
    if (output)
      ret = write;
    break;
  case FS_TEST:
    for (int i = 0; i < limit; i++)
      debugf("%c", in[i]);
    break;
  }
  return ret;
}

void fsReadFullFile(OpenFile *file, uint8_t *out) {
  switch (file->mountPoint->filesystem) {
  case FS_FAT32:
    fsRead(file, out, fsGetFilesize(file));
    break;
  case FS_TEST:
    fsRead(file, out, fsGetFilesize(file));
    break;
  }
}

#define SEEK_SET 0  // start + offset
#define SEEK_CURR 1 // current + offset
#define SEEK_END 2  // end + offset
int fsUserSeek(uint32_t fd, int offset, int whence) {
  OpenFile *file = fsUserNodeFetch(currentTask, fd);
  int       target = offset;
  if (whence == SEEK_SET)
    target += 0;
  else if (whence == SEEK_CURR)
    target += file->pointer;
  else if (whence == SEEK_END)
    target += fsGetFilesize(file);

  bool ret = false;
  switch (file->mountPoint->filesystem) {
  case FS_FAT32:
    // "hack" because fatfs does not use our pointer
    if (whence == SEEK_CURR)
      target += f_tell((FIL *)file->dir);
    ret = f_lseek(file->dir, target);
    break;
  }
  if (!ret)
    return -1;
  return target;
}
