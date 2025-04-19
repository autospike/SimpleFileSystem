/**
 * COMP 7500 Project 4 - Simple File System
 * William Baker
 * Auburn University
 * 
 * cpmfsys.c is part of a standalone C program called cpmRun that simulates a basic CP/M file system.
 * cpmfsys.c implements the following operations: directory listing, deletion, renaming, and free-block management.
 * 
 * This program was compiled using a make file that should be present in the same directory as this file.
 * 
 */

#include "ctype.h"
#include "cpmfsys.h"
#include "diskSimulator.h"

bool freeList[NUM_BLOCKS];

//Create and populate a DirStructType from a specific exten in block 0
DirStructType  *mkDirStruct(int index,uint8_t *e) {
  DirStructType *d =malloc(sizeof(DirStructType));
  if (!d) return NULL;

  //Get starting byte of extent
  uint8_t *extent = e + index * EXTENT_SIZE;
  
  //Status byte
  d->status = extent[0];

  //Bytes 1-8, filename
  memcpy(d->name, &extent[1], 8);
  d->name[8] = '\0';
  
  //Bytes 9-11, extension
  memcpy(d->extension, &extent[9], 3);
  d->extension[3] = '\0';

  //Bytes 12-15, metadata
  d->XL = extent[12]; //Low bits
  d->BC = extent[13]; //Bytes past last full sector
  d->XH = extent[14]; //High bits
  d->RC = extent[15]; //Number of sectors in last block
  
  //Bytes 16-31, data block pointers
  memcpy(d->blocks, &extent[16], BLOCKS_PER_EXTENT);

  return d;
}

//Write a DirStructType back into block 0 at a given extent index
void writeDirStruct(DirStructType *d,uint8_t index, uint8_t *e) {
  uint8_t *extent = e + index * EXTENT_SIZE;
  
  //Update status
  extent[0] = d->status;

  //Update name and extension fields
  memcpy(&extent[1], d->name, 8);
  memcpy(&extent[9], d->extension, 3);

  //Update metadata fields
  extent[12] = d->XL;
  extent[13] = d->BC;
  extent[14] = d->XH;
  extent[15] = d->RC;
  
  //Update block pointers
  memcpy(&extent[16], d->blocks, BLOCKS_PER_EXTENT);
}

//Populate free list by checking all used blocks listed in extents
void makeFreeList() {
  //Initially mark all as free
  memset(freeList, true, sizeof(freeList));
  //Reserve block 0
  freeList[0] = false;

  uint8_t block0[BLOCK_SIZE];
  blockRead(block0, 0);

  //Go through each extent in block 0
  for (int i = 0; i < BLOCK_SIZE; i += EXTENT_SIZE) {
    //If extent is used...
    if (block0[i] != 0xE5) {
      //...mark each block in the extent as used
      for (int j =16; j < EXTENT_SIZE; j++) {
        uint8_t blockNum = block0[i + j];
        if (blockNum != 0) {
          //Mark as in use
          freeList[blockNum] = false;
        }
      }
    }
  }
}

//Print free list, '*' is used, '.' is free
void printFreeList() {
  printf("FREE BLOCK LIST: (* means in-use)\n");
  for (int i = 0; i < 16; i++) {
    printf("%02x: ", i * 16);
    for (int j = 0; j < 16; j++) {
      int state = i * 16 + j;
      printf("%c ", freeList[state] ? '.' : '*');
    }
    printf("\n");
  }
}

//Print all directory entries, just the names and sizes
void cpmDir() {
  uint8_t block0[BLOCK_SIZE];
  blockRead(block0, 0);

  printf("DIRECTORY LISTING\n");
  //Loop through all extents
  for (int i = 0; i < BLOCK_SIZE / EXTENT_SIZE; i++) {
    DirStructType *d = mkDirStruct(i, block0);
    //If valid...
    if (d && d->status != 0xE5) {
      //...count how many blocks are allocated
      int cnt = 0;
      for (int j = 0; j < BLOCKS_PER_EXTENT; j++) {
        if (d->blocks[j] != 0) {
          cnt++;
        }
      }
      //Full blocks are 1024 bytes
      int full_bytes = (cnt > 1 ? (cnt - 1) * BLOCK_SIZE : 0);
      //Calculate lst block bytes
      int last_bytes = d->RC * 128 + d->BC;
      int size = full_bytes + last_bytes;

      //Remove trailing spaces from name
      char nameTrim[9];
      int nl = strcspn(d->name, " ");
      memcpy(nameTrim, d->name, nl);
      nameTrim[nl] = '\0';

      //Remove trailing spaces from extension
      char extTrim[4];
      int el = strcspn(d->extension, " ");
      memcpy(extTrim, d->extension, el);
      extTrim[el] = '\0';
      
      //Print name and filesize
      printf("%s.%s %d\n", nameTrim, extTrim, size);
    }
    free(d);
  }
}

//Ensure full file name follows 8.3 format and contains valid chars
bool checkLegalName(char *name) {
  //Ensure full name exists and that it is between 1 and 12 chars
  if (!name || strlen(name) < 1 || strlen(name) > 12) {
    return false;
  }

  //Separate name and extension
  char *dot = strchr(name, '.');
  int nameLen = dot ? (dot - name) : strlen(name);
  int extLen = dot ? strlen(dot + 1) : 0;

  //Check name and extension lengths
  if (nameLen < 1 || nameLen > 8 || extLen > 3) {
    return false;
  }

  //Ensure name only contains alphanumeric chars
  for (int i  = 0; i < nameLen; i++) {
    if (!isalnum(name[i])) {
      return false;
    }
  }

  //Ensure extension only contains alphanumeric chars
  for (int i = 0; i < extLen; i++) {
    if (!isalnum(dot[i + 1])) {
      return false;
    }
  }

  return true;
}

//Find the extent index of a certain file
int findExtentWithName(char *name, uint8_t *block0) {
  //Ensure file name is valid
  if (!checkLegalName(name)) {
    return -1;
  }

  //Prepare padded name buffer
  char fileName[9];
  memset(fileName, ' ', 8);
  fileName[8] = '\0';
  
  //Prepare padded extension buffer
  char fileExt[4];
  memset(fileExt, ' ', 3);
  fileExt[3] = '\0';

  //Split at dot
  char *dot = strchr(name, '.');
  if (dot) {
    int nl = dot - name;
    if (nl > 8) {
      nl = 8;
    }
    memcpy(fileName, name, nl);
    //Everything after the dot
    int el = strlen(dot + 1);
    if (el > 3) {
      el = 3;
    }  
    memcpy(fileExt, dot + 1, el);
  } 
  else {
    int nl = strlen(name);
    if (nl > 8) {
      nl = 8;
    }
    memcpy(fileName, name, nl);
  }

  //Compare name and extenstion against each extent
  for (int i = 0; i < BLOCK_SIZE / EXTENT_SIZE; i++) {
    DirStructType *d = mkDirStruct(i, block0);
    if (d && d->status != 0xE5
      && strncmp(d->name, fileName, 8) == 0
      && strncmp(d->extension, fileExt, 3) == 0) {
        free(d);
        return i;
    }
    free(d);
  }  
  //File not found
  return -1;
}

//Mark file as deleted and free associated blocks
int cpmDelete(char *fileName) {
  uint8_t block0[BLOCK_SIZE];
  blockRead(block0, 0);

  //Find extent where the file is
  int index = findExtentWithName(fileName, block0);
  if (index == -1) {
    return -1;
  }

    //Mark extent as unused
  DirStructType *d = mkDirStruct(index, block0);
  d->status = 0xE5;

  //Free each block in the extent
  for (int i = 0; i < BLOCKS_PER_EXTENT; i++) {
    if (d->blocks[i] != 0) {
      freeList[d->blocks[i]] = true;
    }
  }

  writeDirStruct(d, index, block0);
  blockWrite(block0, 0);
  free(d);
  return 0;
}

//Rename file
int cpmRename(char *oldName, char *newName) {
  //Ensure file name is valid
  if (!checkLegalName(oldName) || !checkLegalName(newName)) {
    return -2;
  }

  uint8_t block0[BLOCK_SIZE]; blockRead(block0, 0);
  char key[13];
  
  //Check for trailing dot
  if (!strchr(oldName, '.')) {
    snprintf(key, sizeof key, "%s.", oldName);
  }
  else {
    strncpy(key, oldName, sizeof key);
  }
  
  //Check of old file exists
  int oldIdx = findExtentWithName(key, block0);
  if (oldIdx == -1) {
    return -1;
  }
  
  //Check if new name already exists
  int newIdx = findExtentWithName(newName, block0);
  if (newIdx != -1) {
    return -3;
  }

  DirStructType *d = mkDirStruct(oldIdx, block0);
  //Clear existing name and extension fields
  memset(d->name, ' ', 8);
  memset(d->extension, ' ', 3);
  
  //Parse new name
  char *dot2 = strchr(newName, '.');
  if (dot2) {
    int nl2 = dot2 - newName; if (nl2 > 8) nl2 = 8;
    memcpy(d->name, newName, nl2);
    int el2 = strlen(dot2 + 1); if (el2 > 3) el2 = 3;
    memcpy(d->extension, dot2 + 1, el2);
  } 
  else {
    int nl2 = strlen(newName); if (nl2 > 8) nl2 = 8;
    memcpy(d->name, newName, nl2);
  }
  
  writeDirStruct(d, oldIdx, block0);
  blockWrite(block0, 0);
  free(d);
  return 0;
}
