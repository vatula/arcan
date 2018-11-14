# arcan-net

This tool is the development sandbox for the arcan-net bridge used to link
single clients over a network.

DISCLAIMER: This is in an early stage and absolutely not production ready now
or anytime soon. Treat it like a sample of what is to come during the 0.6-
series of releases - the real version will depend on the rendering protocol
that will be derived from a packing format of the lua API, the serialization
format used in TUI and A/V encoding formats to be decided (likely
h264-lowlatency for game content, AV1 for bufferable media if hw gets fast
enough, zstd for uncompressed blob transfers, Open3DGC for 3dvobj data etc).

There are two modes to building this tool, a 'simple' (arcan-netpipe) and
the "real" but defunct (arcan-net) and disabled.

Arcan-netpipe uses some unspecified channel for transmission, e.g. piping via
SSH and so on. As such, is has rather low performance and the communication is
otherwise unprotected. It is valuable for testing, development and
debugging/fault-injection - and for quick and dirty bridging. It can also only
bridge a single client per instance.

Arcan-net is just stubs/defunct experiments at the moment, the intention is to
build on UDT as a low-latency UDP based transport but will not be given any
consideration until netpipe is fully working as the a12- state machine manager
need to be really robust before going further.

# Use

Arcan netpipe version (testing example, act as a local MiM proxy):

    ./arcan-netpipe -t -s mycon &
		ARCAN_CONNPATH=mycon afsrv_terminal

Forwarding a local client:

    ./arcan-netpipe -s mycon -x ssh user@host arcan-netpipe -c -s localcon

Note that a connection point need to be specified on both sides.

Forwarding a remote client:

    ./arcan-netpipe -c -x ssh user@host arcan-netpipe -s mycon -x afsrv_terminal

Or you can manually setup fifos, client/server etc.:

    mkfifo c cl_in
		mkfifo c cl_out
		cat cl_in | arcan-netpipe -c | cl_out
		cat cl_out |arcan-netpipe -s mycon | cl_in

# Todo

This subproject will stretch until the end of the 0.6 series topic, with some
sharing likely to be done with the afsrv\_net (i.e. the underlying protocol,
and state machine, a12, used along with some service discovery feature and
better scripting API integration).

Milestone 1 - basic features (0.5.x)

- [x] Basic API
- [x] Control
- [x] Netpipe working
- [x] Uncompressed Video / Video delta
- [ ] Uncompressed Audio / Audio delta
- [ ] Compressed Video
- [ ] Raw binary descriptor transfers
- [ ] Subsegments

Milestone 2 - closer to useful (0.6.x)

- [ ] Better / source specific compression
- [ ] TUI- text channel
- [ ] A / V / E interleaving
- [ ] Progressive encoding
- [ ] Accelerated encoding of gpu-handles
- [ ] Traffic monitoring tools
- [ ] UDT based carrier (full- proxy client)

Milestone 3 - big stretch (0.6.x)

- [ ] curve25519 key exchange
- [ ] Stream-ciper
- [ ] 'ALT' arcan-lwa interfacing
- [ ] ZSTD
- [ ] Open3DGC
- [ ] Congestion control / dynamic encoding parameters
- [ ] Side-channel Resilience
- [ ] Local discovery Mechanism
- [ ] Merge into arcan-net
- [ ] Special provisions for agp/alt channels

# Security/Safety

Right now, there's barely any (there will be though) - a lot of the other
quality problems should be solved first, i.e. audio / video format encoding,
event packing format and so on.

The only required part right now is that there is a shared authentication key
file (0..64 bytes) that has been preshared over some secure channel, ssh is a
good choice. This key is used for building individual packet MACs.

ALL DATA IS BEING SENT IN PLAINTEXT, ANYONE ON THE NETWORK CAN SEE WHAT YOU DO.

# Protocol

Two primary transports are intended, one tunneled through ssh and via the
arcan-netpipe tool. The other is using UDT or a similar low-latency UDP
transport.

Each arcan segment correlates to a 'channel' that can be multiplexed over
one of these transports, with a sequence number used as a synchronization
primitive for re-linearization. For each channel, a number of streams can
be defined, each with a unique 32-bit identifier. Multiple streams can be
in flight at the same time, and can be dynamically cancelled.

Encryption is built on preauthenticated curve25519 with blake2-aes128-ctr
for MAC and cipher. Each message begins with a 16 byte MAC, keyed with the
input auth-key for the first message, then payload prefixed with the MAC
from the last message.

Each message has the outer structure of :

 |---------------------|
 |    16 byte MAC      |
 |---------------------|
 | 4 byte sequence     |-
 | 1 byte command code | - } encrypted block
 | command-code data   | -
 |---------------------|-

The payload is encrypt-then-MAC (if there has been a session key negotiated)
The stream-cipher server-to-client starts at [8bIV,8bCTR(0)] and the
client-to-server starts at [8bIV,8bCTR(1<<32)] with session rekey/rebuild.

After the MAC comes a 4 byte LSB unsigned sequence number, and then a 1 byte
command code, then a number of command-specific bytes. The sequence number
does not necessarily increment between messages as v/a/b streams might be
multipart.

The different message types are:

1. control (128b fixed)
2. event, tied to the format of arcan\_shmif\_evpack()
3. vstream-data
4. astream-data
5. bstream-data

Event frames are likely to be interleaved between vframes/aframes/bstreams
to avoid input- bubbles, and there is only one a/v/b type of transfer going
on at any one time. The rest are expected to block- the source or queue up.

If the most significant bit of the sequence number is set, it is a discard-
message used to mess with side-channel analysis for cases where bandwidth
is a lesser concern than security.

## Control (1)
- [0..7]    last-seen seqnr : uint64
- [8..15]   entropy : uint8[8]
- [16]      channel-id : uint8
- [17]      command : uint8

The last-seen are used both as a timing channel and to determine drift.
If the two sides start drifting outside a certain window, measures to reduce
bandwidth should be taken, including increasing compression parameters,
lowering sample- and frame- rates, cancelling ongoing frames, merging /
delaying input events, scaling window sizes, ... If they still keep
drifting, show a user notice and destroy the channel.

### command = 0, hello
First message sent of the channel, it's rude not to say hi.

### command = 1, shutdown
The nice way of saying that everything is about to be shut down.

### command = 2, encryption negotiation
- K(auth) : uint8[16]
- K(pub) : uint8[32]
- IV : uint8[8]

Used to generate ephemeral keys and rekey after this sequence number.

### command = 3, channel rekey
- K(auth) : uint8[16]
- K(sym) : uint8[32]
- IV : uint8[8]

Switch into rekeying state, opens a small window where MAC failures will be
tested against both the expected and the new auth key, after the first package
that authenticated against the new key, this one is used. Rekeying is initiated
by the server side, authoritative and infrequent; typically early and then
after a certain number of bytes.

### command = 4, stream-cancel
- stream-id : uint32

This command carries a 4 byte stream ID, which is the counter shared by all
bstream, vstream and astreams.

### command = 5, channel negotiation
- sequence number : uint64
- segkind : uint8

This maps to subsegment requests and bootstraps the keys, rendezvous, etc.
needed to initiate a new channel as part of a subsegment setup.

### command - 6, command failure
- sequence number : uint64
- segkind : uint8

### command - 7, define vstream
- [18..21] : stream-id: uint32
- [22    ] : format: uint8
- [23..24] : surfacew: uint16
- [25..26] : surfaceh: uint16
- [27..28] : startx: uint16 (0..outw-1)
- [29..30] : starty: uint16 (0..outh-1)
- [31..32] : framew: uint16 (outw-startx + framew < outw)
- [33..34] : frameh: uint16 (outh-starty + frameh < outh)
- [35    ] : dataflags: uint8
- [36..39] : length: uint32
- [40..43] : expanded length: uint32
- [44]     : commit: uint8

This defines a new video stream frame. The length- field covers how many bytes
that need to be buffered for the data to be decoded. This can be chunked up
into 1..n packages, depending on interleaving and so on. The vstream counter
increases incrementally and is shared between the v/a/b streams.

Outw/Outh can change frequently (corresponds to window resize).

If there is already an active frame, it will be cancelled out and replaced
with this one - similar to if a stream-cancel command had been issued.

### command - 8, define astream
incomplete

### command - 9, define bstream
incomplete

##  Event (2), fixed length
- sequence number : uint64
- channel-id : uint8

This follows the packing format provided by the SHMIF- libraries themselves,
which have their own pack/unpack/versioning routines. When the SHMIF event
model itself is finalized, it will be added to the documentation here.

## Vstream-data (3), Astream-data (4), Bstream-data (5) (variable length)
- channel-id : uint8
- stream-id : uint32
- length : uint32

# Licenses

arcan-net is (c) Bjorn Stahl 2017-2018 and licensed under the 3-clause BSD
license. It is dependent on BLAKE2- (CC or Apache-2.0, see COPYING.BLAKE2)
and on UDT (Apache-2.0 / 3-clause BSD).
