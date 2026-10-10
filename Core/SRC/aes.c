/**
 * @file aes.c
 * @brief Compact table-driven AES-128 (ECB, encryption only).
 *
 * Only compiled when TRES_AES_ENABLE == 1. Implementation notes:
 *  - S-box table + xtime for MixColumns (no full T-table); ~512 B tables.
 *  - Round keys expanded once per call into a 176 B stack buffer; no heap.
 *  - Matches FIPS-197 Appendix B vector:
 *      key = 2B7E151628AED2A6ABF7158809CF4F3C
 *      pt  = 3243F6A8885A308D313198A2E0370734
 *      ct  = 3925841D02DC09FBDC118597196A0B32
 */
#if ( TRES_AES_ENABLE == 1 )

#include "aes.h"
#include <string.h>   /* memcpy */

/*============================================================================*/
/* AES S-box (FIPS-197 tab. 5)                                                 */
/*============================================================================*/
static const uint8_t s_sbox[ 256 ] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Round constants (FIPS-197 tab. 6). */
static const uint8_t s_rcon[ 10 ] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

/* xtime: multiply by x in GF(2^8) mod x^8+x^4+x^3+x+1. */
static uint8_t aes_xtime( uint8_t b )
{
    return (uint8_t)( (uint8_t)( b << 1 ) ^ ( ( b & 0x80u ) ? 0x1Bu : 0x00u ) );
}

void Aes128_Encrypt( const uint8_t key[ 16 ],
                     const uint8_t in[ 16 ],
                     uint8_t out[ 16 ] )
{
    uint8_t rk[ 176 ];            /* expanded key: 11 x 16 B                  */
    uint8_t st[ 16 ];             /* state                                     */
    uint8_t t[ 16 ];              /* ShiftRows staging                         */
    uint8_t i, r;

    /*--- Key expansion ------------------------------------------------------*/
    /* Round 0 = raw key. Word i (4 B) of the expanded key is folded from the  */
    /* previous word; every 4th word goes through the g() transform.          */
    {
        uint8_t w[ 4 ];
        memcpy( rk, key, 16 );
        for ( i = 4u ; i < 44u ; i++ ) {
            uint8_t *prev = &rk[ ( i - 1 ) * 4 ];
            uint8_t *cur  = &rk[ i * 4 ];
            if ( ( i & 3u ) == 0u ) {
                /* g(): RotWord -> SubWord -> XOR Rcon */
                w[ 0 ] = s_sbox[ prev[ 1 ] ] ^ s_rcon[ ( i / 4 ) - 1 ];
                w[ 1 ] = s_sbox[ prev[ 2 ] ];
                w[ 2 ] = s_sbox[ prev[ 3 ] ];
                w[ 3 ] = s_sbox[ prev[ 0 ] ];
            } else {
                /* plain: copy previous word */
                w[ 0 ] = prev[ 0 ]; w[ 1 ] = prev[ 1 ];
                w[ 2 ] = prev[ 2 ]; w[ 3 ] = prev[ 3 ];
            }
            cur[ 0 ] = w[ 0 ] ^ rk[ ( i - 4 ) * 4     ];
            cur[ 1 ] = w[ 1 ] ^ rk[ ( i - 4 ) * 4 + 1 ];
            cur[ 2 ] = w[ 2 ] ^ rk[ ( i - 4 ) * 4 + 2 ];
            cur[ 3 ] = w[ 3 ] ^ rk[ ( i - 4 ) * 4 + 3 ];
        }
    }

    /*--- Initial AddRoundKey ------------------------------------------------*/
    for ( i = 0u ; i < 16u ; i++ ) {
        st[ i ] = in[ i ] ^ rk[ i ];
    }

    /*--- 9 full rounds -------------------------------------------------------*/
    for ( r = 1u ; r <= 9u ; r++ ) {
        /* SubBytes */
        for ( i = 0u ; i < 16u ; i++ ) {
            st[ i ] = s_sbox[ st[ i ] ];
        }
        /* ShiftRows (column-major state) */
        memcpy( t, st, 16 );
        st[  1 ] = t[  5 ]; st[  5 ] = t[  9 ]; st[  9 ] = t[ 13 ]; st[ 13 ] = t[  1 ];
        st[  2 ] = t[ 10 ]; st[ 10 ] = t[  2 ]; st[  6 ] = t[ 14 ]; st[ 14 ] = t[  6 ];
        st[  3 ] = t[ 15 ]; st[ 15 ] = t[ 11 ]; st[ 11 ] = t[  7 ]; st[  7 ] = t[  3 ];
        /* MixColumns */
        for ( i = 0u ; i < 16u ; i += 4u ) {
            uint8_t a0 = st[ i     ], a1 = st[ i + 1 ];
            uint8_t a2 = st[ i + 2 ], a3 = st[ i + 3 ];
            st[ i     ] = (uint8_t)( aes_xtime( a0 ) ^ aes_xtime( a1 ) ^ a1 ^ a2 ^ a3 );
            st[ i + 1 ] = (uint8_t)( a0 ^ aes_xtime( a1 ) ^ aes_xtime( a2 ) ^ a2 ^ a3 );
            st[ i + 2 ] = (uint8_t)( a0 ^ a1 ^ aes_xtime( a2 ) ^ aes_xtime( a3 ) ^ a3 );
            st[ i + 3 ] = (uint8_t)( aes_xtime( a0 ) ^ a0 ^ a1 ^ a2 ^ aes_xtime( a3 ) );
        }
        /* AddRoundKey */
        for ( i = 0u ; i < 16u ; i++ ) {
            st[ i ] ^= rk[ r * 16 + i ];
        }
    }

    /*--- Final round (no MixColumns) ------------------------------------------*/
    for ( i = 0u ; i < 16u ; i++ ) {
        st[ i ] = s_sbox[ st[ i ] ];
    }
    memcpy( t, st, 16 );
    st[  1 ] = t[  5 ]; st[  5 ] = t[  9 ]; st[  9 ] = t[ 13 ]; st[ 13 ] = t[  1 ];
    st[  2 ] = t[ 10 ]; st[ 10 ] = t[  2 ]; st[  6 ] = t[ 14 ]; st[ 14 ] = t[  6 ];
    st[  3 ] = t[ 15 ]; st[ 15 ] = t[ 11 ]; st[ 11 ] = t[  7 ]; st[  7 ] = t[  3 ];
    for ( i = 0u ; i < 16u ; i++ ) {
        st[ i ] ^= rk[ 160 + i ];
    }

    memcpy( out, st, 16 );
}

#endif /* TRES_AES_ENABLE == 1 */