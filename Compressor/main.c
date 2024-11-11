#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

static uint32_t LittleToBig(uint32_t i)  {
	return ((i >> 24) | (i << 24) | ((i & 0x00FF0000) >> 8) | ((i & 0x0000FF00) << 8));
}

#define min(a,b)    (((a)<(b))?(a):(b))
#define max(a,b)    (((a)>(b))?(a):(b))


typedef struct CxiHuffNode_ {
	uint16_t sym;
	uint16_t symMin; //had space to spare, maybe make searches a little simpler
	uint16_t symMax;
	uint16_t nRepresent;
	int freq;
	struct CxiHuffNode_ *left;
	struct CxiHuffNode_ *right;
} CxiHuffNode;

//struct for representing tokenized LZ data
typedef struct CxiLzToken_ {
	uint8_t isReference;
	union {
		uint8_t symbol;
		struct {
			uint16_t length;
			uint16_t distance;
		};
	};
} CxiLzToken;

typedef struct CxiLzNode_ {
	CxiLzToken token;
	uint32_t weight;
} CxiLzNode;

typedef struct BITSTREAM_ {
	uint32_t *bits;
	int nWords;
	int nBitsInLastWord;
	int nWordsAlloc;
	int length;
} BITSTREAM;



// ----- bit stream

static void CxiBitStreamCreate(BITSTREAM *stream) {
	stream->nWords = 0;
	stream->length = 0;
	stream->nBitsInLastWord = 32;
	stream->nWordsAlloc = 16;
	stream->bits = (uint32_t *) calloc(stream->nWordsAlloc, 4);
}

static void CxiBitStreamFree(BITSTREAM *stream) {
	free(stream->bits);
}

static void CxiBitStreamWrite(BITSTREAM *stream, int bit) {
	if (stream->nBitsInLastWord == 32) {
		stream->nBitsInLastWord = 0;
		stream->nWords++;
		if (stream->nWords > stream->nWordsAlloc) {
			int newAllocSize = (stream->nWordsAlloc + 2) * 3 / 2;
			stream->bits = realloc(stream->bits, newAllocSize * 4);
			stream->nWordsAlloc = newAllocSize;
		}
		stream->bits[stream->nWords - 1] = 0;
	}

	stream->bits[stream->nWords - 1] |= (bit << (31 - stream->nBitsInLastWord));
	stream->nBitsInLastWord++;
	stream->length++;
}

static void *CxiBitStreamGetBytes(BITSTREAM *stream, int wordAlign, int beBytes, int beBits, unsigned int *size) {
	//allocate buffer
	unsigned int outSize = stream->nWords * 4;
	if (!wordAlign) {
		//nBitsInLast word is 32 if last word is full, 0 if empty.
		if (stream->nBitsInLastWord <= 24) outSize--;
		if (stream->nBitsInLastWord <= 16) outSize--;
		if (stream->nBitsInLastWord <=  8) outSize--;
		if (stream->nBitsInLastWord <=  0) outSize--;
	}
	unsigned char *outbuf = (unsigned char *) calloc(outSize, 1);
	if (outbuf == NULL) return NULL;

	//this function handles converting byte and bit orders from the internal
	//representation. Internally, we store the bit sequence as an array of
	//words, where the first bits are inserted at the most significant bit.
	//

	for (unsigned int i = 0; i < outSize; i++) {
		int byteShift = 8 * ((beBytes) ? (3 - (i % 4)) : (i % 4));
		uint32_t word = stream->bits[i / 4];
		uint8_t byte = (word >> byteShift) & 0xFF;

		//if little endian bit order, swap here
		if (!beBits) {
			uint8_t temp = byte;
			byte = 0;
			for (int j = 0; j < 8; j++) byte |= ((temp >> j) & 1) << (7 - j);
		}
		outbuf[i] = byte;
	}

	*size = outSize;
	return outbuf;
}

static void CxiBitStreamWriteBitsBE(BITSTREAM *stream, uint32_t bits, int nBits) {
	for (int i = 0; i < nBits; i++) CxiBitStreamWrite(stream, (bits >> (nBits - 1 - i)) & 1);
}


// ----- LZ search code

static unsigned int CxiCompareMemory(const unsigned char *b1, const unsigned char *b2, unsigned int nMax, unsigned int nAbsoluteMax) {
	if (nMax > nAbsoluteMax) nMax = nAbsoluteMax;

	if (nAbsoluteMax >= nMax) {
		//compare nAbsoluteMax bytes, do not perform any looping.
		unsigned int nSame = 0;
		while (nAbsoluteMax > 0) {
			if (*(b1++) != *(b2++)) break;
			nAbsoluteMax--;
			nSame++;
		}
		return nSame;
	} else {
		//compare nMax bytes, then repeat the comparison until nAbsoluteMax is 0.
		unsigned int nSame = 0;
		while (nAbsoluteMax > 0) {

			//compare strings once, incrementing b2 (but keeping b1 fixed since it's repeating)
			unsigned int nSameThis = 0;
			for (unsigned int i = 0; i < nMax; i++) {
				if (b1[i] == *(b2++)) {
					nSameThis++;
				} else {
					break;
				}
			}

			nAbsoluteMax -= nSameThis;
			nSame += nSameThis;
			if (nSameThis < nMax) break; //failed comparison
		}
		return nSame;
	}
}

static int CxiLzConfirmMatch(const unsigned char *buffer, unsigned int size, unsigned int pos, unsigned int distance, unsigned int length) {
	(void) size;
	
	//confirm that the <length, distance> pair matches length bytes at pos in the buffer.
	if (length <= distance) {
		//if the source and destination don't overlap, simple memcmp
		return memcmp(buffer + pos, buffer + pos - distance, length) == 0;
	}
	
	//else, length > distance, compare the leading bytes repeating
	unsigned int nTotalCompare = length;
	unsigned int compareSrc = pos;
	while (nTotalCompare) {
		//get number of byte to compare this run
		unsigned int nCompare = nTotalCompare;
		if (nCompare > distance) nCompare = distance;
		
		if (memcmp(buffer + compareSrc, buffer + pos - distance, nCompare) != 0) return 0;
		nTotalCompare -= nCompare;
		compareSrc += nCompare;
	}
	return 1;
}

static unsigned int CxiSearchLZ(const unsigned char *buffer, unsigned int size, unsigned int curpos, unsigned int minDistance, unsigned int maxDistance, unsigned int maxLength, unsigned int *pDistance) {
	//nProcessedBytes = curpos
	unsigned int nBytesLeft = size - curpos;

	//the maximum distance we can search backwards is limited by how far into the buffer we are. It won't
	//make sense to a decoder to copy bytes from before we've started.
	if (maxDistance > curpos) maxDistance = curpos;

	//keep track of the biggest match and where it was
	unsigned int biggestRun = 0, biggestRunIndex = 0;

	//the longest string we can match, including repetition by overwriting the source.
	unsigned int nMaxCompare = maxLength;
	if (nMaxCompare > nBytesLeft) nMaxCompare = nBytesLeft;

	//begin searching backwards.
	for (unsigned int j = minDistance; j <= maxDistance; j++) {
		//compare up to 0xF bytes, at most j bytes.
		unsigned int nCompare = maxLength;
		if (nCompare > j) nCompare = j;
		if (nCompare > nMaxCompare) nCompare = nMaxCompare;

		unsigned int nMatched = CxiCompareMemory(buffer - j, buffer, nCompare, nMaxCompare);
		if (nMatched > biggestRun) {
			biggestRun = nMatched;
			biggestRunIndex = j;
			if (biggestRun == nMaxCompare) break;
		}
	}

	*pDistance = biggestRunIndex;
	return biggestRun;
}

static unsigned int CxiSearchLZRestricted(const unsigned char *buffer, unsigned int size, unsigned int curpos, const unsigned int *distances, int nDistances, unsigned int maxLength, unsigned int *pDistance) {
	if (nDistances == 0) {
		*pDistance = 0;
		return 0;
	}
	
	//nProcessedBytes = curpos
	unsigned int nBytesLeft = size - curpos;

	//the maximum distance we can search backwards is limited by how far into the buffer we are. It won't
	//make sense to a decoder to copy bytes from before we've started.
	unsigned int maxDistance = distances[nDistances - 1];
	if (maxDistance > curpos) maxDistance = curpos;

	//keep track of the biggest match and where it was
	unsigned int biggestRun = 0, biggestRunIndex = 0;

	//the longest string we can match, including repetition by overwriting the source.
	unsigned int nMaxCompare = maxLength;
	if (nMaxCompare > nBytesLeft) nMaxCompare = nBytesLeft;

	//begin searching backwards.
	for (int i = 0; i < nDistances; i++) {
		unsigned int j = distances[i];
		if (j > maxDistance) break;
		
		//compare up to 0xF bytes, at most j bytes.
		unsigned int nCompare = maxLength;
		if (nCompare > j) nCompare = j;
		if (nCompare > nMaxCompare) nCompare = nMaxCompare;

		unsigned int nMatched = CxiCompareMemory(buffer - j, buffer, nCompare, nMaxCompare);
		if (nMatched > biggestRun) {
			biggestRun = nMatched;
			biggestRunIndex = j;
			if (biggestRun == nMaxCompare) break;
		}
	}

	*pDistance = biggestRunIndex;
	return biggestRun;
}



// ----- Huffman tree code

#define ISLEAF(n) ((n)->left==NULL&&(n)->right==NULL)

static void CxiHuffmanInit(CxiHuffNode *nodes, unsigned int nNodes) {
	memset(nodes, 0, nNodes * 2 * sizeof(CxiHuffNode));
	for (unsigned int i = 0; i < nNodes; i++) {
		nodes[i].symMin = nodes[i].symMax = nodes[i].sym = i;
		nodes[i].nRepresent = 1;
	}
}

static int CxiHuffmanNodeComparator(const void *p1, const void *p2) {
	return ((CxiHuffNode *) p2)->freq - ((CxiHuffNode *) p1)->freq;
}

static void CxiHuffmanMakeShallowFirst(CxiHuffNode *node) {
	if (ISLEAF(node)) return;
	if (node->left->nRepresent > node->right->nRepresent) {
		CxiHuffNode *left = node->left;
		node->left = node->right;
		node->right = left;
	}
	CxiHuffmanMakeShallowFirst(node->left);
	CxiHuffmanMakeShallowFirst(node->right);
}

static int CxiHuffmanHasSymbol(CxiHuffNode *node, uint16_t sym) {
	if (ISLEAF(node)) return node->sym == sym;
	if (sym < node->symMin || sym > node->symMax) return 0;
	CxiHuffNode *left = node->left;
	CxiHuffNode *right = node->right;
	return CxiHuffmanHasSymbol(left, sym) || CxiHuffmanHasSymbol(right, sym);
}

static void CxiHuffmanWriteSymbol(BITSTREAM *bits, uint16_t sym, CxiHuffNode *tree) {
	if (ISLEAF(tree)) return;
	
	CxiHuffNode *left = tree->left;
	CxiHuffNode *right = tree->right;
	if (CxiHuffmanHasSymbol(left, sym)) {
		CxiBitStreamWrite(bits, 0);
		CxiHuffmanWriteSymbol(bits, sym, left);
	} else {
		CxiBitStreamWrite(bits, 1);
		CxiHuffmanWriteSymbol(bits, sym, right);
	}
}

static void CxiHuffmanConstructTree(CxiHuffNode *nodes, int nNodes) {
	//sort by frequency, then cut off the remainder (freq=0).
	qsort(nodes, nNodes, sizeof(CxiHuffNode), CxiHuffmanNodeComparator);
	for (int i = 0; i < nNodes; i++) {
		if (nodes[i].freq == 0) {
			nNodes = i;
			break;
		}
	}

	//unflatten the histogram into a huffman tree. 
	int nRoots = nNodes;
	int nTotalNodes = nNodes;
	while (nRoots > 1) {
		//copy bottom two nodes to just outside the current range
		CxiHuffNode *srcA = nodes + nRoots - 2;
		CxiHuffNode *destA = nodes + nTotalNodes;
		memcpy(destA, srcA, sizeof(CxiHuffNode));

		CxiHuffNode *left = destA;
		CxiHuffNode *right = nodes + nRoots - 1;
		CxiHuffNode *branch = srcA;

		branch->freq = left->freq + right->freq;
		branch->sym = 0;
		branch->left = left;
		branch->right = right;
		branch->symMin = min(left->symMin, right->symMin);
		branch->symMax = max(right->symMax, left->symMax);
		branch->nRepresent = left->nRepresent + right->nRepresent; //may overflow for root, but the root doesn't really matter for this

		nRoots--;
		nTotalNodes++;
		qsort(nodes, nRoots, sizeof(CxiHuffNode), CxiHuffmanNodeComparator);
	}

	//just to be sure, make sure the shallow node always comes first
	CxiHuffmanMakeShallowFirst(nodes);
}

static unsigned int CxiHuffmanGetNodeDepth(CxiHuffNode *tree, unsigned int sym) {
	if (tree->left == NULL) {
		//this is the node
		return 0;
	}
	
	if (CxiHuffmanHasSymbol(tree->left, sym)) {
		return CxiHuffmanGetNodeDepth(tree->left, sym) + 1;
	} else if (CxiHuffmanHasSymbol(tree->right, sym)) {
		return CxiHuffmanGetNodeDepth(tree->right, sym) + 1;
	} else {
		//!!!
		return 0;
	}
}

typedef struct CxiHuffSymbolInfo_ {
	uint16_t sym;
	uint16_t depth;
} CxiHuffSymbolInfo;

static int CxiHuffmanCountSymbolsOver(CxiHuffNode *tree, unsigned int nMin) {
	if (tree->left == NULL) {
		return tree->sym >= nMin;
	}
	return CxiHuffmanCountSymbolsOver(tree->left, nMin) + CxiHuffmanCountSymbolsOver(tree->right, nMin);
}

static CxiHuffSymbolInfo *CxiHuffmanEnumerateSymbolInfoInternal(CxiHuffNode *tree, CxiHuffSymbolInfo *buf, int depth, unsigned int nMin) {
	if (tree->left == NULL) {
		if (tree->sym >= nMin) {
			buf->sym = tree->sym;
			buf->depth = depth;
			buf++;
		}
	} else {
		//run recursion
		buf = CxiHuffmanEnumerateSymbolInfoInternal(tree->left, buf, depth + 1, nMin);
		buf = CxiHuffmanEnumerateSymbolInfoInternal(tree->right, buf, depth + 1, nMin);
	}
	return buf;
}

static int CxiHuffSymbolInfoComparator(const void *e1, const void *e2) {
	const CxiHuffSymbolInfo *d1 = (const CxiHuffSymbolInfo *) e1;
	const CxiHuffSymbolInfo *d2 = (const CxiHuffSymbolInfo *) e2;
	
	if (d1->sym < d2->sym) return -1;
	if (d1->sym > d2->sym) return +1;
	return 0;
}

static CxiHuffSymbolInfo *CxiHuffmanEnumerateSymbolInfo(CxiHuffNode *tree, int *pCount, unsigned int nMin) {
	int nNode = CxiHuffmanCountSymbolsOver(tree, nMin);
	CxiHuffSymbolInfo *buf = (CxiHuffSymbolInfo *) calloc(nNode, sizeof(CxiHuffSymbolInfo));
	if (buf == NULL) return NULL;
	
	CxiHuffmanEnumerateSymbolInfoInternal(tree, buf, 0, nMin);
	
	//sort
	qsort(buf, nNode, sizeof(*buf), CxiHuffSymbolInfoComparator);
	
	*pCount = nNode;
	return buf;
}


// ----- ASH code

static void CxiAshEnsureTreeElements(CxiHuffNode *nodes, int nNodes, int nMinNodes) {
	//count nodes
	int nPresent = 0;
	for (int i = 0; i < nNodes; i++) {
		if (nodes[i].freq) nPresent++;
	}
	
	//have sufficient nodes?
	if (nPresent >= nMinNodes) return;
	
	//add dummy nodes
	for (int i = 0; i < nNodes; i++) {
		if (nodes[i].freq == 0) {
			nodes[i].freq = 1;
			nPresent++;
			if (nPresent >= nMinNodes) return;
		}
	}
}

static void CxiAshWriteTree(BITSTREAM *stream, CxiHuffNode *nodes, int nBits) {
	if (nodes->left != NULL) {
		//
		CxiBitStreamWrite(stream, 1);
		CxiAshWriteTree(stream, nodes->left, nBits);
		CxiAshWriteTree(stream, nodes->right, nBits);
	} else {
		//write value
		CxiBitStreamWrite(stream, 0);
		CxiBitStreamWriteBitsBE(stream, nodes->sym, nBits);
	}
}

static CxiLzToken *CxiAshTokenize(const unsigned char *buffer, unsigned int size, int nSymBits, int nDstBits, unsigned int *pnTokens) {
	unsigned int nTokens = 0, tokenBufferSize = 16;
	CxiLzToken *tokenBuffer = (CxiLzToken *) calloc(tokenBufferSize, sizeof(CxiLzToken));
	if (tokenBuffer == NULL) return NULL;
	
	//
	unsigned int curpos = 0;
	while (curpos < size) {
		//ensure buffer capacity
		if (nTokens + 1 > tokenBufferSize) {
			tokenBufferSize = (tokenBufferSize + 2) * 3 / 2;
			tokenBuffer = (CxiLzToken *) realloc(tokenBuffer, tokenBufferSize * sizeof(CxiLzToken));
		}
		
		//search backwards
		unsigned int length, distance;
		length = CxiSearchLZ(buffer, size, curpos, 1, (1 << nDstBits), (1 << nSymBits) - 1 - 0x100 + 3, &distance);
		
		CxiLzToken *token = &tokenBuffer[nTokens++];
		if (length >= 3) {
			token->isReference = 1;
			token->length = length;
			token->distance = distance;
			
			buffer += length;
			curpos += length;
		} else  {
			token->isReference = 0;
			token->symbol = *(buffer++);
			curpos++;
		}
	}
	
	*pnTokens = nTokens;
	tokenBuffer = realloc(tokenBuffer, nTokens * sizeof(CxiLzToken));
	return tokenBuffer;
}

static void CxiAshGenHuffman(const CxiLzToken *tokens, unsigned int nTokens, CxiHuffNode *symNodes, unsigned int nSymNodes, CxiHuffNode *dstNodes, unsigned int nDstNodes) {
	CxiHuffmanInit(symNodes, nSymNodes);
	CxiHuffmanInit(dstNodes, nDstNodes);
	
	//construct frequency distribution
	for (unsigned int i = 0; i < nTokens; i++) {
		const CxiLzToken *token = &tokens[i];
		if (token->isReference) {
			symNodes[token->length - 3 + 0x100].freq++;
			dstNodes[token->distance - 1].freq++;
		} else {
			symNodes[token->symbol].freq++;
		}
	}
	
	//pre-tree construction: ensure at least two nodes are used
	CxiAshEnsureTreeElements(symNodes, nSymNodes, 2);
	CxiAshEnsureTreeElements(dstNodes, nDstNodes, 2);
	
	//construct trees
	CxiHuffmanConstructTree(symNodes, nSymNodes);
	CxiHuffmanConstructTree(dstNodes, nDstNodes);
}

static unsigned int CxiAshRoundDown(unsigned int sym, unsigned int *vals, unsigned int nVals, int *pIndex) {
	//if sym = 0, return 0 (no element can be equal)
	if (sym == 0) {
		*pIndex = -1;
		return 0;
	}
	
	//check lowest value <=
	unsigned int lo = 0;
	int loIndex = -1;
	for (unsigned int i = 0; i < nVals; i++) {
		if (vals[i] == sym) {
			*pIndex = (int) i;
			return sym;
		}
		
		if (vals[i] < sym) {
			lo = vals[i];
			loIndex = (int) i;
		} else {
			break;
		}
	}
	
	//if lo == 0, no match found, so return 1 with index of -1 (1 is implicitly in the list)
	if (lo == 0) lo = 1;
	*pIndex = loIndex;
	return lo;
}

static CxiLzToken *CxiAshRetokenize(const unsigned char *buffer, unsigned int size, CxiHuffNode *symNodes, CxiHuffNode *dstNodes, unsigned int *pnTokens) {
	//allocate graph
	CxiLzNode *nodes = (CxiLzNode *) calloc(size, sizeof(CxiLzNode));
	if (nodes == NULL) return NULL;
	
	//get a list of allowed distances
	int nLenNodesAvailable, nDstNodesAvailable;
	CxiHuffSymbolInfo *lenInfo = CxiHuffmanEnumerateSymbolInfo(symNodes, &nLenNodesAvailable, 0x100);
	CxiHuffSymbolInfo *dstInfo = CxiHuffmanEnumerateSymbolInfo(dstNodes, &nDstNodesAvailable,     0);
	
	//create array of allowed lengths
	unsigned int *lens = (unsigned int *) calloc(nLenNodesAvailable, sizeof(unsigned int));
	for (int i = 0; i < nLenNodesAvailable; i++) lens[i] = lenInfo[i].sym - 0x100 + 3;
	
	//create array of allowed distances
	unsigned int *dsts = (unsigned int *) calloc(nDstNodesAvailable, sizeof(unsigned int));
	for (int i = 0; i < nDstNodesAvailable; i++) dsts[i] = dstInfo[i].sym + 1;
	
	//scan backwards from end of file
	unsigned int pos = size;
	while (pos-- > 0) {
		//search LZ
		unsigned int length = 0, distance = 0;
		if (nLenNodesAvailable > 0) {
			length = CxiSearchLZRestricted(buffer + pos, size, pos, dsts, nDstNodesAvailable, lens[nLenNodesAvailable - 1], &distance);
		}
		
		//check: length must be in the allowed lengths list.
		int lengthIndex = -1;
		if (length >= 3) {
			length = CxiAshRoundDown(length, lens, nLenNodesAvailable, &lengthIndex);
		} else {
			length = 1;
		}
		
		//NOTE: all byte values that appear in the file will have a symbol associated since they must appear at least once.
		//thus we do not need to check that any byte value exists.
		
		//check length (should store reference?)
		unsigned int weight = 0;
		if (length < 3) {
			//byte literal (can't go lower)
			length = 1;
			
			//compute cost of byte literal
			weight = CxiHuffmanGetNodeDepth(symNodes, buffer[pos]);
			if ((pos + 1) < size) {
				//add next weight
				weight += nodes[pos + 1].weight;
			}
		} else {
			//get cost of selected distance
			unsigned int dstCost = CxiHuffmanGetNodeDepth(dstNodes, distance - 1);
			
			//scan size down
			unsigned int weightBest = UINT_MAX, lengthBest = length; 
			while (length) {
				unsigned int thisWeight;
				
				//compute weight of this length value
				unsigned int thisLengthWeight;
				if (length > 1) {
					//length > 1: symbol (use cost of length symbol)
					thisLengthWeight = lenInfo[lengthIndex].depth;
				} else {
					//length == 1: byte literal (use cost of byte literal)
					thisLengthWeight = CxiHuffmanGetNodeDepth(symNodes, buffer[pos]);
				}
				
				//takes us to end of file? 
				if ((pos + length) == size) {
					//cost is just this node's weight
					thisWeight = thisLengthWeight;
				} else {
					//cost is this node's weight plus the weight of the next node
					CxiLzNode *next = nodes + pos + length;
					thisWeight = thisLengthWeight + next->weight;
				}
				if (thisWeight < weightBest) {
					weightBest = thisWeight;
					lengthBest = length;
				}
				
				//decrement length and round down
				length = CxiAshRoundDown(length - 1, lens, nLenNodesAvailable, &lengthIndex);
			}
			
			length = lengthBest;
			if (length < 3) {
				//byte literal (distance cost is thus now zero since we have no distance component)
				length = 1;
				dstCost = 0;
			} else {
				//we ended up selecting an LZ copy-able length. but did we select the most optimal distance
				//encoding?
				//search possible distances where we can match the string at. We'll take the lowest-cost one.
				for (int i = 0; i < nDstNodesAvailable; i++) {
					unsigned int dst = dsts[i];
					if (dst > pos) break;

					//matching distance, check the cost
					if (dstInfo[i].depth < dstCost) {
						//check matching LZ string...
						if (CxiLzConfirmMatch(buffer, size, pos, dst, length)) {
							dstCost = dstInfo[i].depth;
							distance = dst;
						}
					}
				}
			}
			weight = weightBest + dstCost;
		}
		
		//write node
		if (length >= 3) {
			nodes[pos].token.isReference = 1;
			nodes[pos].token.distance = distance;
			nodes[pos].token.length = length;
		} else {
			nodes[pos].token.isReference = 0;
			nodes[pos].token.symbol = buffer[pos];
		}
		nodes[pos].weight = weight;
	}
	
	free(lens);
	free(dsts);
	free(lenInfo);
	free(dstInfo);
	
	//convert graph into node array
	unsigned int nTokens = 0;
	pos = 0;
	while (pos < size) {
		CxiLzNode *node = nodes + pos;
		nTokens++;
		
		if (node->token.isReference) pos += node->token.length;
		else pos++;
	}
	
	CxiLzToken *tokens = (CxiLzToken *) calloc(nTokens, sizeof(CxiLzToken));
	if (tokens == NULL) {
		free(nodes);
		return NULL;
	}
	
	{
		pos = 0;
		unsigned int i = 0;
		while (pos < size) {
			CxiLzNode *node = nodes + pos;
			
			memcpy(&tokens[i++], &node->token, sizeof(CxiLzToken));
			
			if (node->token.isReference) pos += node->token.length;
			else pos++;
		}
		free(nodes);
	}
	
	*pnTokens = nTokens;
	return tokens;
}

unsigned char *CxCompressAsh(const unsigned char *buffer, unsigned int size, int nSymBits, int nDstBits, unsigned int nPasses, unsigned int *compressedSize) {
	//allocate tree structures
	int nSymNodes = (1 << nSymBits);
	int nDstNodes = (1 << nDstBits);
	CxiHuffNode *symNodes = (CxiHuffNode *) calloc(nSymNodes * 2, sizeof(CxiHuffNode));
	CxiHuffNode *dstNodes = (CxiHuffNode *) calloc(nDstNodes * 2, sizeof(CxiHuffNode));
	if (symNodes == NULL || dstNodes == NULL) {
		if (symNodes != NULL) free(symNodes);
		if (dstNodes != NULL) free(dstNodes);
		return NULL;
	}
	
	//tokenize
	unsigned int nTokens = 0;
	CxiLzToken *tokens = CxiAshTokenize(buffer, size, nSymBits, nDstBits, &nTokens);
	if (tokens == NULL) {
		free(symNodes);
		free(dstNodes);
		return NULL;
	}
	
	CxiAshGenHuffman(tokens, nTokens, symNodes, nSymNodes, dstNodes, nDstNodes);
	
	// ----------------------------------------------------------------------------------------------
	//    Herein lies the really expensive operations (both memory and time).
	// ----------------------------------------------------------------------------------------------
	
	//iterate on adjusting the frequency distribution and traversing the encoding space
	for (unsigned int i = 0; i < nPasses; i++) {
		//discard tokenized sequence. 
		free(tokens);

		//re-tokenize
		tokens = CxiAshRetokenize(buffer, size, symNodes, dstNodes, &nTokens);
		if (tokens == NULL) {
			free(symNodes);
			free(dstNodes);
			return NULL;
		}
		
		//regenerate huffman tree due to changes in frequency distribution
		CxiAshGenHuffman(tokens, nTokens, symNodes, nSymNodes, dstNodes, nDstNodes);
	}
	
	// ----------------------------------------------------------------------------------------------
	//    End of super intense operations
	// ----------------------------------------------------------------------------------------------
	
	//init streams
	BITSTREAM symStream, dstStream;
	CxiBitStreamCreate(&symStream);
	CxiBitStreamCreate(&dstStream);
	
	//first, write huffman trees.
	CxiAshWriteTree(&symStream, symNodes, nSymBits);
	CxiAshWriteTree(&dstStream, dstNodes, nDstBits);
	
	//write data stream
	for (unsigned int i = 0; i < nTokens; i++) {
		CxiLzToken *token = &tokens[i];
		
		if (token->isReference) {
			CxiHuffmanWriteSymbol(&symStream, token->length - 3 + 0x100, symNodes);
			CxiHuffmanWriteSymbol(&dstStream, token->distance - 1, dstNodes);
		} else {
			CxiHuffmanWriteSymbol(&symStream, token->symbol, symNodes);
		}
	}
	
	//free node and tree structs
	free(tokens);
	free(symNodes);
	free(dstNodes);
	
	//encode data output
	unsigned int symStreamSize = 0;
	unsigned int dstStreamSize = 0;
	void *symBytes = CxiBitStreamGetBytes(&symStream, 1, 1, 1, &symStreamSize);
	void *dstBytes = CxiBitStreamGetBytes(&dstStream, 1, 1, 1, &dstStreamSize);
	
	//write data out
	unsigned char *out = (unsigned char *) calloc(0xC + symStreamSize + dstStreamSize, 1);
	{
		//write header
		uint32_t header[3];
		header[0] = 0x30485341;    // 'ASH0'
		header[1] = LittleToBig(size);
		header[2] = LittleToBig(0xC + symStreamSize);
		memcpy(out, header, sizeof(header));
		
		//write streams
		memcpy(out + sizeof(header), symBytes, symStreamSize);
		memcpy(out + sizeof(header) + symStreamSize, dstBytes, dstStreamSize);
	}
	free(symBytes);
	free(dstBytes);
	
	//free stuff
	CxiBitStreamFree(&symStream);
	CxiBitStreamFree(&dstStream);
	
	*compressedSize = 0xC + symStreamSize + dstStreamSize;
	return out;
}

int main(int argc, char **argv) {
	//syntax: ashcomp <infile> [option...]
	if (argc < 2) {
		puts("Usage: ashcomp <infile> [option...]\n");
		puts("Options:");
		puts(" -o <f> Specify output file path");
		puts(" -d <n> Specify distance tree bits   (default: 11)");
		puts(" -l <n> Specify length tree bits     (default:  9)");
		puts(" -c <n> Specify compression strength (0=default, 1=moderate, 2=high)");
		puts("");
		return 1;
	}
	
	//process command line string
	const char *outpath = NULL;
	const char *inpath = argv[1];
	int nSymBits = 9, nDistBits = 11, compPasses = 0; //defaults
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			i++;
			if (i < argc) outpath = argv[i];
		} else if (strcmp(argv[i], "-d") == 0) {
			i++;
			if (i < argc) nDistBits = atoi(argv[i]);
		} else if (strcmp(argv[i], "-l") == 0) {
			i++;
			if (i < argc) nSymBits = atoi(argv[i]);
		} else if (strcmp(argv[i], "-c") == 0) {
			i++;
			if (i < argc) compPasses = atoi(argv[i]);
		}
	}
	
	FILE *fp = fopen(inpath, "rb");
	if (fp == NULL) {
		fprintf(stderr, "Could not open %s for read access.\n", inpath);
		return 1;
	}
	
	//read in file
	unsigned int size;
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	
	unsigned char *inbuf = malloc(size);
	
	//validate file size
	if (inbuf == NULL || size > 0xFFFFFF) {
		if (inbuf != NULL) free(inbuf);
		fclose(fp);
		fprintf(stderr, "File size (%u bytes) exceeds maximum allowed size.\n", size);
		return 1;
	}
	fread(inbuf, size, 1, fp);
	fclose(fp);
	
	//compress
	unsigned int outSize;
	unsigned char *out = CxCompressAsh(inbuf, size, nSymBits, nDistBits, compPasses, &outSize);
	free(inbuf);
	
	if (out == NULL) {
		fprintf(stderr, "Compression failure.\n");
		return 1;
	}
	
	//get output file name (if not specified)
	char strbuf[1024];
	if (outpath == NULL) {
		//if no output specified, append .ash
		sprintf(strbuf, "%s.ash", argv[1]);
		outpath = strbuf;
	}
	
	fp = fopen(outpath, "wb");
	if (fp == NULL) {
		fprintf(stderr, "Could not open %s for write access.\n", outpath);
		return 1;
	}
	
	fwrite(out, 1, outSize, fp);
	free(out);
	fclose(fp);
	return 0;
}
