// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime-installed raw Chrome-EC passthrough for the CIX Sky1 EC.
 *
 * Arg0: complete host request buffer (0xDA prefix + ec_host_request + data),
 *       already checksummed by the caller.
 * Arg1: number of response bytes to read from the EC.
 *
 * Returns Package { TRAS transport status, response buffer }.
 */
DefinitionBlock ("", "SSDT", 2, "CIXQFN", "ECRAW", 1)
{
    External (\_SB.EC0, DeviceObj)
    External (\_SB.EC0.TRAS, MethodObj)

    Method (\_SB.EC0.XRAW, 2, Serialized)
    {
        Name (RBUF, Buffer (0x40) {})
        Name (ROUT, Package (0x02) { Ones, Zero })
        ROUT [Zero] = \_SB.EC0.TRAS (Arg0, SizeOf (Arg0), RBUF, Arg1)
        ROUT [One] = RBUF
        Return (ROUT)
    }
}
