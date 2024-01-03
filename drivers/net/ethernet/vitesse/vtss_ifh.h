/* Copyright (c) 2019 Microsemi Corporation

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

enum {
	SOC_LUTON,
	SOC_SERVAL1,
	SOC_OCELOT,
	SOC_JAGUAR2,
	SOC_SERVALT,
	SOC_SPARX5,
	SOC_LAN966X,
	SOC_LAN969X,
};

enum {
	IFH_ID_LUTON   = 0x00,	/* No IFH_ID in Luton26. Must use unassigned */
	IFH_ID_SERVAL1 = 0x05,
	IFH_ID_OCELOT  = 0x0a,
	IFH_ID_JAGUAR2 = 0x07,
	IFH_ID_SERVALT = 0x09,
	IFH_ID_SPARX5 = 0x0b,
	IFH_ID_LAN966X = 0xd,
	IFH_ID_LAN969X = 0xe,
};

#define IFH_LEN_LUTON			8
#define IFH_OFFS_PORT_MASK_LUTON	32

#define IFH_LEN_SERVAL1			16
#define IFH_OFFS_PORT_MASK_SERVAL1	57

#define IFH_LEN_OCELOT			16
#define IFH_OFFS_PORT_MASK_OCELOT	56

#define IFH_LEN_JAGUAR2			28
#define IFH_OFFS_PORT_MASK_JAGUAR2	128

#define IFH_LEN_SPARX5			36
#define IFH_OFFS_PORT_MASK_SPARX5	386

#define IFH_LEN_LAN966X			28
#define IFH_OFFS_PORT_MASK_LAN966X	62

#define IFH_LEN_LAN969X			36
#define IFH_OFFS_PORT_MASK_LAN969X	386
