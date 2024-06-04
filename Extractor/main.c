#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;


#define TREE_RIGHT    0x80000000
#define TREE_LEFT     0x40000000
#define TREE_VAL_MASK 0x3FFFFFFF


static void AbortInvalidData(int line) {
	fprintf(stderr, "!!! Invalid compressed data.\n");
	fprintf(stderr, "    Line %d\n", line);
	exit(1);
}

#define CX_ASSERT(x)   if(!(x))AbortInvalidData(__LINE__)


u32 BigToLittle32(u32 i)  {
	return ((i >> 24) | (i << 24) | ((i & 0x00FF0000) >> 8) | ((i & 0x0000FF00) << 8));
}


typedef struct CxBitReader_ {
	const u8 *srcp;
	u32 size;
	u32 srcpos;
	u32 word;
	u32 bitCapacity;
} CxBitReader;

static void CxBitReaderFeedWord(CxBitReader *reader) {
	//bounds check the source data to prevent a buffer overrun
	CX_ASSERT(reader->srcpos + 4 <= reader->size);
	
	reader->word = BigToLittle32(*(const u32 *) (reader->srcp + reader->srcpos));
	reader->bitCapacity = 0;
	reader->srcpos += 4;
}

void CxBitReaderInit(CxBitReader *reader, const u8 *src, u32 size, u32 startpos) {
	reader->srcp = src;
	reader->size = size;
	reader->srcpos = startpos;
	CxBitReaderFeedWord(reader);
}

int CxBitReaderReadBit(CxBitReader *reader) {
	int bit = reader->word >> 31;
	if (reader->bitCapacity == 31) {
		CxBitReaderFeedWord(reader);
	} else {
		reader->bitCapacity++;
		reader->word <<= 1;
	}
	return bit;
}

u32 CxBitReaderReadBits(CxBitReader *reader, int nBits) {
	u32 bits;
	u32 next = reader->bitCapacity + nBits;
	if (next <= 32) {
		bits  = reader->word >> (32 - nBits);
		if (next != 32) {
			reader->word <<= nBits;
			reader->bitCapacity += nBits;
		} else {
			CxBitReaderFeedWord(reader);
		}
	} else {
		bits = reader->word >> (32 - nBits);
		CxBitReaderFeedWord(reader);
		bits |= (reader->word >> (64 - next));
		reader->word <<= (next - 32);
		reader->bitCapacity = next - 32;
	}
	return bits;
}

u32 CxAshReadTree(CxBitReader *reader, int width, u32 *leftTree, u32 *rightTree) {
	u32 *work = (u32 *) calloc(2 * (1 << width), sizeof(u32));
	
	u32 r23 = (1 << width);
	u32 symRoot;
	u32 nNodes = 0;
	do {
		if (CxBitReaderReadBit(reader)) {
			*(work++) = r23 | TREE_RIGHT;
			*(work++) = r23 | TREE_LEFT;
			nNodes += 2;
			r23++;
		} else {
			symRoot = CxBitReaderReadBits(reader, width);
			do {
				u32 nodeval = *--work;
				u32 idx = nodeval & TREE_VAL_MASK;
				nNodes--;
				if (nodeval & TREE_RIGHT) {
					rightTree[idx] = symRoot;
					symRoot = idx;
				} else {
					leftTree[idx] = symRoot;
					break;
				}
			} while (nNodes > 0);
		}
	} while (nNodes > 0);
	
	free(work);
	
	return symRoot;
}


u8 *CxUncompressAsh(const u8 *inbuf, u32 size, u32 *outlen, int symBits, int distBits) {
	u32 uncompSize = BigToLittle32(*(u32 *) (inbuf + 4)) & 0x00FFFFFF;
	u32 outSize = uncompSize;
	
	u8 *outbuf = calloc(uncompSize, 1);
	u8 *destp = outbuf;
	
	CxBitReader reader, reader2;
	CxBitReaderInit(&reader, inbuf, size, BigToLittle32(*(const u32 *) (inbuf + 0x8)));
	CxBitReaderInit(&reader2, inbuf, size, 0xC);
	
	u32 symMax = (1 << symBits);
	u32 distMax = (1 << distBits);
	
	//HACK, pointer to RAM
	u32 *symLeftTree   = calloc(2 * symMax - 1, sizeof(u32));
	u32 *symRightTree  = calloc(2 * symMax - 1, sizeof(u32));
	u32 *distLeftTree  = calloc(2 * distMax - 1, sizeof(u32));
	u32 *distRightTree = calloc(2 * distMax - 1, sizeof(u32));
	
	u32 symRoot, distRoot;
	symRoot = CxAshReadTree(&reader2, symBits, symLeftTree, symRightTree);
	distRoot = CxAshReadTree(&reader, distBits, distLeftTree, distRightTree);
	
	//main uncompress loop
	do {
		u32 sym = symRoot;
		while (sym >= symMax) {
			if (!CxBitReaderReadBit(&reader2)) {
				sym = symLeftTree[sym];
			} else {
				sym = symRightTree[sym];
			}
		}
		
		if (sym < 0x100) {
			*(destp++) = sym;
			uncompSize--;
		} else {
			u32 distsym = distRoot;
			while (distsym >= distMax) {
				if (!CxBitReaderReadBit(&reader)) {
					distsym = distLeftTree[distsym];
				} else {
					distsym = distRightTree[distsym];
				}
			}
			
			u32 copylen = (sym - 0x100) + 3;
			const u8 *srcp = destp - distsym - 1;
			CX_ASSERT(copylen <= uncompSize);             //assert length valid
			CX_ASSERT((destp - outbuf) >= (distsym + 1)); //assert source valid
			
			uncompSize -= copylen;
			while (copylen--) {
				*(destp++) = *(srcp++);
			}
		}
	} while (uncompSize > 0);
	
	free(symLeftTree);
	free(symRightTree);
	free(distLeftTree);
	free(distRightTree);
	
	*outlen = outSize;
	return outbuf;
}



int main(int argc, char **argv) {
	//syntax: ashdec <infile> [option...]
	if (argc < 2) {
		puts("Usage: ashdec <infile> [option...]\n");
		puts("Options:");
		puts(" -o <f> Specify output file path");
		puts(" -d <n> Specify distance tree bits  (default: 11)");
		puts(" -l <n> Specify length tree bits    (default:  9)");
		puts("");
		return 1;
	}
	
	//process command line string
	const char *outpath = NULL;
	const char *inpath = argv[1];
	int nSymBits = 9, nDistBits = 11; //defaults
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			i++;
			if (i < argc) outpath = argv[i];
		} else if (strcmp(argv[i], "-d") == 0) {
			i++;
			if (i < argc) nDistBits = atoi(argv[i]);
		} else if (strcmp(argv[i], "-l") == 0) {
			i++;
			if (i < argc) nDistBits = atoi(argv[i]);
		}
	}
	
	FILE *fp = fopen(inpath, "rb");
	if (fp == NULL) {
		fprintf(stderr, "Could not open %s for read access.\n", inpath);
		return 1;
	}

	char magic[4];
	fread(magic, sizeof(magic), 1, fp);
	if (memcmp(magic, "ASH", 3)) {
		printf("This is not a valid ASH file\n");
		return 1;
	}

	//read in file
	u32 size;
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	u8 *inbuf = malloc(size);
	fread(inbuf, 1, size, fp);
	fclose(fp);
	
	//try decompress
	u32 uncompSize;
	u8 *uncomp = CxUncompressAsh(inbuf, size, &uncompSize, nSymBits, nDistBits);

	//get output file name (if not specified)
	char strbuf[1024];
	if (outpath == NULL) {
		//if no output specified, append .arc
		sprintf(strbuf, "%s.arc", argv[1]);
		outpath = strbuf;
	}
	
	fp = fopen(outpath, "wb");
	if (fp == NULL) {
		fprintf(stderr, "Could not open %s for write access.\n", outpath);
		return 1;
	}

	fwrite(uncomp, 1, uncompSize, fp);
	free(uncomp);
	fclose(fp);
	return 0;
}
