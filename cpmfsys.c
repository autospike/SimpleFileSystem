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
  d->status = extent[0];

  //Copy name and extension, add null terminator
  memcpy(d->name, &extent[1], 8);
  d->name[8] = '\0';
  memcpy(d->extension, &extent[9], 3);
  d->extension[3] = '\0';

  //Copy metadata and block usage info
  d->XL = extent[12];
  d->BC = extent[13];
  d->XH = extent[14];
  d->RC = extent[15];
  memcpy(d->blocks, &extent[16], BLOCKS_PER_EXTENT);

  return d;
}

//Write a DirStructType back into block 0 at a given extent index
void writeDirStruct(DirStructType *d,uint8_t index, uint8_t *e) {
  uint8_t *extent = e + index * EXTENT_SIZE;
  extent[0] = d->status;

  //Copy name and extention back to extent
  memcpy(&extent[1], d->name, 8);
  memcpy(&extent[9], d->extension, 3);

  //Copy metadata and block usage
  extent[12] = d->XL;
  extent[13] = d->BC;
  extent[14] = d->XH;
  extent[15] = d->RC;
  memcpy(&extent[16], d->blocks, BLOCKS_PER_EXTENT);
}

//Populate free list by checking all used blocks listed in extents
void makeFreeList() {
  memset(freeList, true, sizeof(freeList));
  //Block 0 is used for the directory
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

  printf("DIRETORY LISTING\n");
  //Loop through all extents
  for (int i = 0; i < BLOCK_SIZE / EXTENT_SIZE; i++) {
    DirStructType *d = mkDirStruct(i, block0);
    //If valid...
    if (d && d->status != 0xE5) {
      //...print filename and size
      int size = ((d->RC - 1) * 128) + d->BC;
      printf("%s.%s %d\n", d->name, d->extension, size);
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

//Find block location of file
int findExtentWithName(char *name, uint8_t *block0) {
  if (!checkLegalName(name)) {
    return -1;
  }

  char fileName[9] = "         ";
  char fileExt[4] = "    ";
  sscanf(name, "%8[^.].%3s", fileName, fileExt);

  //Compare name and extenstion against each extent
  for (int i = 0; i <BLOCK_SIZE / EXTENT_SIZE; i++) {
    DirStructType *d = mkDirStruct(i, block0);
    if (d && d->status != 0xE5
      && strncmp(d->name, fileName, 8) ==0
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

  int index = findExtentWithName(fileName, block0);
  if (index == -1) {
    return -1;
  }

  DirStructType *d = mkDirStruct(index, block0);
  d->status = 0xE5;

  //Go through each free block
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
  //Ensure old and new names are valid
  if (!checkLegalName(oldName) || !checkLegalName(newName)) {
    return -2;
  }

  uint8_t block0[BLOCK_SIZE];
  blockRead(block0, 0);

  //Ensure file with old name exists
  int oldIndex = findExtentWithName(oldName, block0);
  if (oldIndex == -1) {
    return -1;
  }

  //Ensure new name is not already in use
  int newIndex = findExtentWithName(newName, block0);
  if (newIndex != -1) {
    return -3;
  }

  DirStructType *d = mkDirStruct(oldIndex, block0);

  //Clear and overwrite name and extension fields
  memset(d->name, ' ', 8);
  memset(d->extension, ' ', 3);
  sscanf(newName, "%8[^.].%3s", d->name, d->extension);

  writeDirStruct(d, oldIndex, block0);
  blockWrite(block0, 0);
  free(d);
  return 0;
}
