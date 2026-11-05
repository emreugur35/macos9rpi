/*
 *  extfs_stub.cpp - External file system (host directory as Mac volume)
 *                   is not supported in the bare-metal port yet.
 */

#include "sysdeps.h"
#include "extfs.h"

void ExtFSInit(void)
{
}

void ExtFSExit(void)
{
}

void InstallExtFS(void)
{
}

int16 ExtFSComm(uint16 message, uint32 paramBlock, uint32 globalsPtr)
{
	return -50;	// paramErr
}

int16 ExtFSHFS(uint32 vcb, uint16 selectCode, uint32 paramBlock, uint32 globalsPtr, int16 fsid)
{
	return -50;	// paramErr
}

// Ethernet reset hook (no networking in the bare-metal port)
void ether_reset(void)
{
}
