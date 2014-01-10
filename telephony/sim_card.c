/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include "android/utils/debug.h"
#include "qemu-common.h"
#include "sim_card.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define  DEBUG  0

#if DEBUG
#  define  D_ACTIVE  VERBOSE_CHECK(modem)
#  define  D(...)   do { if (D_ACTIVE) fprintf( stderr, __VA_ARGS__ ); } while (0)
#else
#  define  D(...)   ((void)0)
#endif

/* set ENABLE_DYNAMIC_RECORDS to 1 to enable dynamic records
 * for now, this is an experimental feature that needs more testing
 */
#define  ENABLE_DYNAMIC_RECORDS  0

#define  A_SIM_PIN_SIZE  4
#define  A_SIM_PUK_SIZE  8

#define  SIM_FILE_RECORD_ABSOLUTE_MODE  4

// @see TS 102.221 section 10.2.1 Status conditions returned by the UICC.
/* Normal processing */
// Normal ending of the command - sw1='90', sw2='00'.
#define  SIM_RESPONSE_NORMAL_ENDING         "+CRSM: 144,0"

/* Execution errors */
// sw1='64', sw2='00', No information given, state of non-volatile memory unchanged.
#define  SIM_RESPONSE_EXECUTION_ERROR       "+CRSM: 100,0"

/* Checking errors */
// sw1='67', sw2='00', Wrong length.
#define  SIM_RESPONSE_WRONG_LENGTH          "+CRSM: 103,0"

/* Wrong parameters */
// sw1='6A', sw2='81', Function not supported.
#define  SIM_RESPONSE_FUNCTION_NOT_SUPPORT  "+CRSM: 106,129"
// sw1='6A', sw2='82', File not found.
#define  SIM_RESPONSE_FILE_NOT_FOUND        "+CRSM: 106,130"
// sw1='6A', sw2='83', Record not found
#define  SIM_RESPONSE_RECORD_NOT_FOUND      "+CRSM: 106,131"
// sw1='6A', sw2='86', Incorrect parameters P1 to P2.
#define  SIM_RESPONSE_INCORRECT_PARAMETERS  "+CRSM: 106,134"

typedef union SimFileRec_ SimFileRec, *SimFile;

typedef struct ASimCardRec_ {
    ASimStatus  status;
    char        pin[ A_SIM_PIN_SIZE+1 ];
    char        puk[ A_SIM_PUK_SIZE+1 ];
    int         pin_retries;
    int         port;
    int         instance_id;

    char        out_buff[ 256 ];
    int         out_size;

    SimFile     efs;

} ASimCardRec;

static ASimCardRec  _s_card[MAX_GSM_DEVICES];

ASimCard
asimcard_create(int port, int instance_id)
{
    ASimCard  card    = &_s_card[instance_id];
    card->status      = A_SIM_STATUS_READY;
    card->pin_retries = 0;
    strncpy( card->pin, "0000", sizeof(card->pin) );
    strncpy( card->puk, "12345678", sizeof(card->puk) );
    card->port = port;
    card->instance_id = instance_id;
    return card;
}

void
asimcard_destroy( ASimCard  card )
{
    /* nothing really */
    card=card;
}

static __inline__ int
asimcard_ready( ASimCard  card )
{
    return card->status == A_SIM_STATUS_READY;
}

ASimStatus
asimcard_get_status( ASimCard  sim )
{
    return sim->status;
}

void
asimcard_set_status( ASimCard  sim, ASimStatus  status )
{
    sim->status = status;
}

const char*
asimcard_get_pin( ASimCard  sim )
{
    return sim->pin;
}

const char*
asimcard_get_puk( ASimCard  sim )
{
    return sim->puk;
}

void
asimcard_set_pin( ASimCard  sim, const char*  pin )
{
    strncpy( sim->pin, pin, A_SIM_PIN_SIZE );
    sim->pin_retries = 0;
}

void
asimcard_set_puk( ASimCard  sim, const char*  puk )
{
    strncpy( sim->puk, puk, A_SIM_PUK_SIZE );
    sim->pin_retries = 0;
}


int
asimcard_check_pin( ASimCard  sim, const char*  pin )
{
    if (sim->status != A_SIM_STATUS_PIN   &&
        sim->status != A_SIM_STATUS_READY )
        return 0;

    if ( !strcmp( sim->pin, pin ) ) {
        sim->status      = A_SIM_STATUS_READY;
        sim->pin_retries = 0;
        return 1;
    }

    if (sim->status != A_SIM_STATUS_READY) {
        if (++sim->pin_retries == 3)
            sim->status = A_SIM_STATUS_PUK;
    }
    return 0;
}


int
asimcard_check_puk( ASimCard  sim, const char* puk, const char*  pin )
{
    if (sim->status != A_SIM_STATUS_PUK)
        return 0;

    if ( !strcmp( sim->puk, puk ) ) {
        strncpy( sim->puk, puk, A_SIM_PUK_SIZE );
        strncpy( sim->pin, pin, A_SIM_PIN_SIZE );
        sim->status      = A_SIM_STATUS_READY;
        sim->pin_retries = 0;
        return 1;
    }

    if ( ++sim->pin_retries == 6 ) {
        sim->status = A_SIM_STATUS_ABSENT;
    }
    return 0;
}

typedef enum {
    SIM_FILE_DM = 0,
    SIM_FILE_DF,
    SIM_FILE_EF_DEDICATED,
    SIM_FILE_EF_LINEAR,
    SIM_FILE_EF_CYCLIC
} SimFileType;

typedef enum {
    SIM_FILE_READ_ONLY       = (1 << 0),
    SIM_FILE_NEED_PIN = (1 << 1),
} SimFileFlags;

/* descriptor for a known SIM File */
#define  SIM_FILE_HEAD       \
    SimFileRec*     next;    \
    SimFileRec**    prev;    \
    SimFileType     type;    \
    unsigned short  id;      \
    unsigned short  flags;

typedef struct {
    SIM_FILE_HEAD
} SimFileAnyRec, *SimFileAny;

typedef struct {
    SIM_FILE_HEAD
    cbytes_t   data;
    int        length;
} SimFileEFDedicatedRec, *SimFileEFDedicated;

typedef struct {
    SIM_FILE_HEAD
    byte_t     rec_count;
    byte_t     rec_len;
    cbytes_t   records;
} SimFileEFLinearRec, *SimFileEFLinear;

typedef SimFileEFLinearRec   SimFileEFCyclicRec;
typedef SimFileEFCyclicRec*  SimFileEFCyclic;

typedef union SimFileRec_ {
    SimFileAnyRec          any;
    SimFileEFDedicatedRec  dedicated;
    SimFileEFLinearRec     linear;
    SimFileEFCyclicRec     cyclic;
} SimFileRec, *SimFile;

static int
asimcard_ef_read_dedicated( SimFile ef, char* dst )
{
    if (dst == NULL) {
        D("ERROR: Destination buffer is NULL.\n");
        return -1;
    }

    if (ef == NULL || ef->any.type != SIM_FILE_EF_DEDICATED) {
        D("ERROR: The type of EF is not SIM_FILE_EF_DEDICATED.\n");
        return -1;
    }

    SimFileEFDedicated dedicated = (SimFileEFDedicated) ef;

    gsm_hex_from_bytes(dst, dedicated->data, dedicated->length);
    dst[dedicated->length * 2] = '\0';

    return dedicated->length;
}

static int
asimcard_ef_read_linear( SimFile ef, byte_t record_id, char* dst )
{
    if (dst == NULL) {
        D("ERROR: Destination buffer is NULL.\n");
        return -1;
    }

    if (ef == NULL || ef->any.type != SIM_FILE_EF_LINEAR) {
        D("ERROR: The type of EF is not SIM_FILE_EF_LINEAR.\n");
        return -1;
    }

    if (ef->linear.rec_count < record_id) {
        D("ERROR: Invaild record id.\n");
        return -1;
    }

    SimFileEFLinear linear = (SimFileEFLinear) ef;
    bytes_t         record = linear->records + ((record_id - 1) * linear->rec_len);

    gsm_hex_from_bytes(dst, record, linear->rec_len);
    dst[linear->rec_len * 2] = '\0';

    return linear->rec_len;
}

static SimFile
asimcard_ef_find( ASimCard sim, int id )
{
    SimFile ef = sim->efs;

    for(; ef != NULL; ef = ef->any.next) {
        if (ef->any.id == id) {
            break;
        }
    }

    return ef;
}

static const char*
asimcard_io_read_binary( ASimCard sim, int id, int p1, int p2, int p3 )
{
    char*   out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    if (p1 != 0 || p2 != 0) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    if (ef->any.type != SIM_FILE_EF_DEDICATED) {
        return SIM_RESPONSE_FUNCTION_NOT_SUPPORT;
    }

    if (ef->dedicated.length < p3) {
        return SIM_RESPONSE_WRONG_LENGTH;
    }

    sprintf(out, "%s,", SIM_RESPONSE_NORMAL_ENDING);
    out  += strlen(out);
    if (asimcard_ef_read_dedicated(ef, out) < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }

    return sim->out_buff;
}

static const char*
asimcard_io_read_record( ASimCard sim, int id, int p1, int p2, int p3 )
{
    char*   out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    // We only support ABSOLUTE_MODE
    if (p2 != SIM_FILE_RECORD_ABSOLUTE_MODE || p1 <= 0) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    if (ef->any.type != SIM_FILE_EF_LINEAR) {
        return SIM_RESPONSE_FUNCTION_NOT_SUPPORT;
    }

    if (ef->linear.rec_count < p1) {
        return SIM_RESPONSE_RECORD_NOT_FOUND;
    }

    if (ef->linear.rec_len < p3) {
        return SIM_RESPONSE_WRONG_LENGTH;
    }

    sprintf(out, "%s,", SIM_RESPONSE_NORMAL_ENDING);
    out += strlen(out);
    if (asimcard_ef_read_linear(ef, p1, out) < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }

    return sim->out_buff;
}

#if ENABLE_DYNAMIC_RECORDS
static int sim_file_to_hex( SimFile  file, bytes_t  dst );

static const char*
asimcard_io_get_response( ASimCard sim, int id, int p1, int p2, int p3 )
{
    int    count;
    char*   out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    if (p1 != 0 || p2 != 0 || p3 != 15) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    sprintf(out, "%s,", SIM_RESPONSE_NORMAL_ENDING);
    out  += strlen(out);
    count = sim_file_to_hex(ef, out);
    if (count < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }
    out[count] = 0;

    return sim->out_buff;
}

/* convert a SIM File descriptor into an ASCII string,
   assumes 'dst' is NULL or properly sized.
   return the number of chars, or -1 on error */
static int
sim_file_to_hex( SimFile  file, bytes_t  dst )
{
    SimFileType  type   = file->any.type;
    int          result = 0;

    /* see 9.2.1 in TS 51.011 */
    switch (type) {
        case SIM_FILE_EF_DEDICATED:
        case SIM_FILE_EF_LINEAR:
        case SIM_FILE_EF_CYCLIC:
            {
                if (dst) {
                    int  file_size, perm;

                    memcpy(dst, "0000", 4);  /* bytes 1-2 are RFU */
                    dst += 4;

                    /* bytes 3-4 are the file size */
                    if (type == SIM_FILE_EF_DEDICATED)
                        file_size = file->dedicated.length;
                    else
                        file_size = file->linear.rec_count * file->linear.rec_len;

                    gsm_hex_from_short( dst, file_size );
                    dst += 4;

                    /* bytes 5-6 are the file id */
                    gsm_hex_from_short( dst, file->any.id );
                    dst += 4;

                    /* byte 7 is the file type - always EF, i.e. 0x04 */
                    dst[0] = '0';
                    dst[1] = '4';
                    dst   += 2;

                    /* byte 8 is RFU, except bit 7 for cyclic files, which indicates
                       that INCREASE is allowed. Since we don't support this yet... */
                    dst[0] = '0';
                    dst[1] = '0';
                    dst   += 2;

                    /* byte 9-11 are access conditions */
                    if (file->any.flags & SIM_FILE_READ_ONLY) {
                        if (file->any.flags & SIM_FILE_NEED_PIN)
                            perm = 0x1a;
                        else
                            perm = 0x0a;
                    } else {
                        if (file->any.flags & SIM_FILE_NEED_PIN)
                            perm = 0x11;
                        else
                            perm = 0x00;
                    }
                    gsm_hex_from_byte(dst, perm);
                    memcpy( dst+2, "a0aa", 4 );
                    dst += 6;

                    /* byte 12 is file status, we don't support invalidation */
                    dst[0] = '0';
                    dst[1] = '0';
                    dst   += 2;

                    /* byte 13 is length of the following data, always 2 */
                    dst[0] = '0';
                    dst[1] = '2';
                    dst   += 2;

                    /* byte 14 is struct of EF */
                    dst[0] = '0';
                    if (type == SIM_FILE_EF_DEDICATED) {
                        dst[1] = '0';
                    } else if (type == SIM_FILE_EF_LINEAR) {
                        dst[1] = '1';
                    } else {
                        dst[1] = '3';
                    }
                    dst   += 2;

                    /* byte 15 is lenght of record, or 0 */
                    if (type == SIM_FILE_EF_DEDICATED) {
                        dst[0] = '0';
                        dst[1] = '0';
                    } else {
                        gsm_hex_from_byte( dst, file->linear.rec_len );
                    }
                    dst   += 2;
                }
                result = 30;
            }
            break;

        default:
            result = -1;
    }
    return result;
}
#endif /* ENABLE_DYNAMIC_RECORDS */

const char*
asimcard_io( ASimCard  sim, const char*  cmd )
{
    int  nn;
#if ENABLE_DYNAMIC_RECORDS
    int  command, id, p1, p2, p3;
#endif
    static const struct { const char*  cmd; const char*  answer; } answers[] =
    {
        // CPHS Network Operator Name(6F14):
        //   PLMN Name: "Android"
        // @see Common PCN Handset Specification (Version 4.2) B.4.1.2 Network Operator Name
        { "+CRSM=192,28436,0,0,15", "+CRSM: 144,0,000000146f1404001aa0aa01020000" },
        { "+CRSM=176,28436,0,0,20", "+CRSM: 144,0,416e64726f6964ffffffffffffffffffffffffff" },

        // CPHS Voice message waiting flag(6F11):
        //   Voice Message Waiting Indicator flags:
        //     Line 1: no messages waiting.
        //     Line 2: no messages waiting.
        // @see Common PCN Handset Specification (Version 4.2) B.4.2.3 Voice Message Waiting Flags in the SIM
        { "+CRSM=192,28433,0,0,15", "+CRSM: 144,0,000000016f11040011a0aa01020000" },
        { "+CRSM=176,28433,0,0,1", "+CRSM: 144,0,55" },

        // ICC Identification(2FE2):
        //   Identification number: 89014103211118518720
        // @see 3GPP TS 11.011 section 10.1.1 EFiccid (ICC Identification)
        { "+CRSM=192,12258,0,0,15", "+CRSM: 144,0,0000000a2fe204000fa0aa01020000" },
        { "+CRSM=176,12258,0,0,10", "+CRSM: 144,0,98101430121181157002" },

        // CPHS Call forwarding flags(6F13):
        //   Voice Call forward unconditional flags:
        //     Line 1: no call forwarding message waiting.
        //     Line 2: no call forwarding message waiting.
        // @see Common PCN Handset Specification (Version 4.2) B.4.5 Diverted Call Status Indicator
        { "+CRSM=192,28435,0,0,15", "+CRSM: 144,0,000000016f13040011a0aa01020000" },
        { "+CRSM=176,28435,0,0,1",  "+CRSM: 144,0,55" },

        // SIM Service Table(6F38):
        //   Enabled: 1..4, 7, 9..19, 26, 27, 29, 30, 38, 51..56
        // @see 3GPP TS 51.011 section 10.3.7 EFsst (SIM Service Table)
        { "+CRSM=192,28472,0,0,15", "+CRSM: 144,0,0000000f6f3804001aa0aa01020000" },
        { "+CRSM=176,28472,0,0,15", "+CRSM: 144,0,ff30ffff3f003c0f000c0000f0ff00" },

        // Mailbox Identifier(6FC9):
        //   Mailbox Dialing Number Identifier - Voicemail:      1
        //   Mailbox Dialing Number Identifier - Fax:            no mailbox dialing number associated
        //   Mailbox Dialing Number Identifier - Eletronic Mail: no mailbox dialing number associated
        //   Mailbox Dialing Number Identifier - Other:          no mailbox dialing number associated
        //   Mailbox Dialing Number Identifier - Videomail:      no mailbox dialing number associated
        // @see 3GPP TS 31.102 section 4.2.62 EFmbi (Mailbox Identifier)
        { "+CRSM=192,28617,0,0,15", "+CRSM: 144,0,000000086fc9040011a0aa01020104" },
        { "+CRSM=178,28617,1,4,4",  "+CRSM: 144,0,01000000" },

        // Message Waiting Indication Status(6FCA):
        //   Message Waiting Indicator Status: all inactive
        //   Number of Voicemail Messages Waiting:       0
        //   Number of Fax Messages Waiting:             0
        //   Number of Electronic Mail Messages Waiting: 0
        //   Number of Other Messages Waiting:           0
        //   Number of Videomail Messages Waiting:       0
        // @see 3GPP TS 31.102 section 4.2.63 EFmwis (Message Waiting Indication Status)
        { "+CRSM=192,28618,0,0,15", "+CRSM: 144,0,0000000a6fca040011a0aa01020105" },
        { "+CRSM=178,28618,1,4,5",  "+CRSM: 144,0,0000000000" },

        // Administrative Data(6FAD):
        //   UE Operation mode: normal
        //   Additional information: none
        //   Length of MNC in the IMSI: 3
        // @see 3GPP TS 31.102 section 4.2.18 EFad (Administrative Data)
        { "+CRSM=192,28589,0,0,15", "+CRSM: 144,0,000000046fad04000aa0aa01020000" },
        { "+CRSM=176,28589,0,0,4",  "+CRSM: 144,0,00000003" },

        // EF-IMG (4F20) : Each record of this EF identifies instances of one particular graphical image,
        //                 which graphical image is identified by this EF's record number.
        //   Number of image instance specified by this record:               01
        //   Image instance width 8 points (raster image points):             08
        //   Image instance heigh 8 points  (raster image points):            08
        //   Color image coding scheme:                                       21
        //   Image identifier id of the EF where is store the image instance: 4F02
        //   Offset of the image instance in the 4F02 EF:                     0000
        //   Length of image instance data:                                   0016
        // @see 3GPP TS 51.011 section 10.6.1.1, EF-img
        { "+CRSM=192,20256,1,4,10", "+CRSM: 144,0,000000644f20040000000005020114" },
        { "+CRSM=178,20256,1,4,20", "+CRSM: 144,0,010808214f0200000016ffffffffffffffffffff" },
        { "+CRSM=176,20226,0,0,22", "+CRSM: 144,0,080802030016AAAA800285428142814281528002AAAAFF000000FF000000FF" },
        { "+CRSM=176,20226,0,22,9", "+CRSM: 144,0,0808ff03a59999a5c3ff" },

        // CPHS Information(6F16):
        //   CPHS Phase: 2
        //   CPHS Service Table:
        //     CSP(Customer Service Profile): allocated and activated
        //     Information Numbers:           allocated and activated
        // @see Common PCN Handset Specification (Version 4.2) B.3.1.1 CPHS Information
        { "+CRSM=192,28438,0,0,15", "+CRSM: 144,0,000000026f1604001aa0aa01020000" },
        { "+CRSM=176,28438,0,0,2",  "+CRSM: 144,0,0233" },

        // Service Provider Name(6F46):
        //   Display Condition: 0x1, display network name in HPLMN; display SPN if not in HPLMN.
        //   Service Provider Name: "Android"
        // @see 3GPP TS 31.102 section 4.2.12 EFspn (Service Provider Name)
        // @see 3GPP TS 51.011 section 9.4.4 Referencing Management
        { "+CRSM=192,28486,0,0,15", "+CRSM: 144,0,000000116f4604000aa0aa01020000" },
        { "+CRSM=176,28486,0,0,17", "+CRSM: 144,0,01416e64726f6964ffffffffffffffffff" },

        // Service Provider Display Information(6FCD):
        //   SPDI TLV (tag = 'a3')
        //     SPDI TLV (tag = '80')
        //       PLMN: 234136
        //       PLMN: 46692
        // @see 3GPP TS 31.102 section 4.2.66 EFspdi (Service Provider Display Information)
        // @see 3GPP TS 51.011 section 9.4.4 Referencing Management
        { "+CRSM=192,28621,0,0,15", "+CRSM: 144,0,0000000d6fcd04000aa0aa01020000" },
        { "+CRSM=176,28621,0,0,13", "+CRSM: 144,0,a30b800932643164269fffffff" },

        // PLMN Network Name(6FC5):
        //   FIXME:
        // @see 3GPP TS 31.102 section 4.2.58 EFpnn (PLMN Network Name)
        // @see 3GPP TS 24.008
        { "+CRSM=192,28613,0,0,15", "+CRSM: 144,0,000000f06fc504000aa0aa01020118" },
        { "+CRSM=178,28613,1,4,24", "+CRSM: 144,0,43058441aa890affffffffffffffffffffffffffffffffff" },

        // MSISDN(6F40):
        //   Alpha Identifier: (empty)
        //   Length of BCD number/SSC contents: 7
        //   TON and NPI: 0x81
        //   Dialing Number/SSC String: 15555218135, actual number is "155552"
        //                              + (sim->instance_id + 1) + emulator port,
        //                              e.g. "15555215554" for first sim of first emulator.
        //   Capacity/Configuration 2 Record Identifier: not used
        //   Extension 5 Record Identifier: not used
        // @see 3GPP TS 31.102 section 4.2.26 EFmsisdn (MSISDN)
        { "+CRSM=192,28480,0,0,15", "+CRSM: 144,0,000000806f40040011a0aa01020120" },
        { "+CRSM=178,28480,1,4,32", "+CRSM: 144,0,ffffffffffffffffffffffffffffffffffff07815155258131f5ffffffffffff" },

        // Mailbox Dialing Numbers(6FC7):
        //   Alpha Identifier: "Voicemail"
        //   Length of BCD number/SSC contents: 7
        //   TON and NPI: 0x91
        //   Dialing Number/SSC String: 15552175049
        //   Capacity/Configuration 2 Record Identifier: not used
        //   Extension 6 Record Identifier: not used
        // @see 3GPP TS 31.102 section 4.2.60 EFmbdn (Mailbox Dialing Numbers)
        { "+CRSM=192,28615,0,0,15", "+CRSM: 144,0,000000406fc7040011a0aa01020120" },
        { "+CRSM=178,28615,1,4,32", "+CRSM: 144,0,566f6963656d61696cffffffffffffffffff07915155125740f9ffffffffffff" },

        // Abbreviated Dialling Numbers(6FCA)
        //   Length of BCD number/SSC contents: 7
        //   TON and NPI: 0x81
        // @see 3GPP TS 51.011 section 10.5.1 EFadn
        { "+CRSM=192,28474,0,0,15", "+CRSM: 144,0,000000806f3a040011a0aa01020120" },
        // Alpha Id(Encoded with GSM 8 bit): "Mozilla", Dialling Number: 15555218201
        { "+CRSM=178,28474,1,4,32", "+CRSM: 144,0,4d6f7a696c6c61ffffffffffffffffffffff07815155258102f1ffffffffffff" },
        // Alpha Id(Encoded with UCS2 0x80: "Saßê黃", Dialling Number: 15555218202
        { "+CRSM=178,28474,2,4,32", "+CRSM: 144,0,800053006100df00ea9ec3ffffffffffffff07815155258102f2ffffffffffff" },
        // Alpha Id(Encoded with UCS2 0x81): "Fire 火", Dialling Number: 15555218203
        { "+CRSM=178,28474,3,4,32", "+CRSM: 144,0,8106e04669726520ebffffffffffffffffff07815155258102f3ffffffffffff" },
        // Alpha Id(Encoded with UCS2 0x82): "Huang 黃", Dialling Number: 15555218204
        { "+CRSM=178,28474,4,4,32", "+CRSM: 144,0,82079e804875616e6720c3ffffffffffffff07815155258102f4ffffffffffff" },

        // Cell Broadcast Message Identifier selection(6F45):
        //   CB Message Identifier 1: 45056(B000)
        //   CB Message Identifier 2: 65535(FFFF, not used)
        //   CB Message Identifier 3: 61440(F000, not settable by MMI)
        // @see 3GPP TS 31.102 section 4.2.14 EFcbmi (Cell Broadcast Message Identifier selection)
        { "+CRSM=192,28485,0,0,15", "+CRSM: 144,0,000000066f4504000fa0aa01020000" },
        { "+CRSM=176,28485,0,0,6", "+CRSM: 144,0,b000fffff000" },

        // Cell Broadcast Message Identifier Range selection(6F50):
        //   CB Message Identifier Range 1: 45058..49152(B002..C000)
        //   CB Message Identifier Range 2: 65535..49153(FFFF..C001, should be ignored)
        //   CB Message Identifier Range 3: 49153..65535(C001..FFFF, should be ignored)
        //   CB Message Identifier Range 4: 61442..65280(F002..FF00, not settable by MMI)
        // @see 3GPP TS 31.102 section 4.2.14 EFcbmir (Cell Broadcast Message Identifier Range selection)
        { "+CRSM=192,28496,0,0,15", "+CRSM: 144,0,000000106f5004000fa0aa01020000" },
        { "+CRSM=176,28496,0,0,16", "+CRSM: 144,0,b002c000ffffc001c001fffff002ff00" },

        { NULL, NULL }
    };

    assert( memcmp( cmd, "+CRSM=", 6 ) == 0 );

#if ENABLE_DYNAMIC_RECORDS
    if ( sscanf(cmd, "+CRSM=%d,%d,%d,%d,%d", &command, &id, &p1, &p2, &p3) == 5 ) {
        switch (command) {
            case A_SIM_CMD_GET_RESPONSE:
                return asimcard_io_get_response(sim, id, p1, p2, p3);

            case A_SIM_CMD_READ_BINARY:
                return asimcard_io_read_binary(sim, id, p1, p2, p3);

            case A_SIM_CMD_READ_RECORD:
                return asimcard_io_read_record(sim, id, p1, p2, p3);

            default:
                return SIM_RESPONSE_FUNCTION_NOT_SUPPORT;
        }
    }
#endif

    if (!strcmp("+CRSM=178,28480,1,4,32", cmd)) {
        snprintf( sim->out_buff, sizeof(sim->out_buff),
                  "+CRSM: 144,0,ffffffffffffffffffffffffffffffffffff0781515525%d%d%d%df%dffffffffffff",
                  (sim->port / 1000) % 10,
                  (sim->instance_id + 1),
                  (sim->port / 10) % 10,
                  (sim->port / 100) % 10,
                  sim->port % 10);
        return sim->out_buff;
        }

    for (nn = 0; answers[nn].cmd != NULL; nn++) {
        if ( !strcmp( answers[nn].cmd, cmd ) ) {
            return answers[nn].answer;
        }
    }
    return SIM_RESPONSE_INCORRECT_PARAMETERS;
}

