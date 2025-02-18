#!/usr/bin/env python3

import re
import itertools

from enum import Enum
from collections import namedtuple

# Change this to the rev for the map you want to generate.
sidecar_rev = "B"

class ConnectorKind(Enum):
    CPU = 0
    BACKPLANE = 1
    FRONT_IO = 2

lane_map_rev_a = [
    ('BP0 (A) J21', 8, ['L0*', 'L2*', 'L3', 'L1', 'L7*', 'L6*', 'L5*', 'L4*'], ['L1*', 'L7*', 'L5*', 'L3*', 'L6*', 'L4*', 'L2*', 'L0*']),
    ('BP0 (C) J24', 6, ['L4', 'L3', 'L6*', 'L7*', 'L5*', 'L1', 'L2*', 'L0*'], ['L5', 'L6*', 'L7', 'L4*', 'L1', 'L0*', 'L2*', 'L3*']),
    ('BP1 (A) J19', 7, ['L4', 'L2', 'L6*', 'L0', 'L7', 'L3', 'L5', 'L1*'], ['L3', 'L7*', 'L6', 'L4*', 'L5', 'L2*', 'L1*', 'L0*']),
    ('BP1 (C) J22', 5, ['L7*', 'L4', 'L5*', 'L0*', 'L1*', 'L6*', 'L3*', 'L2*'], ['L5*', 'L6*', 'L7*', 'L2*', 'L1*', 'L0*', 'L4*', 'L3']),
    ('BP2 (A) J25', 3, ['L4', 'L2*', 'L5*', 'L6', 'L7', 'L0*', 'L3', 'L1*'], ['L7', 'L6', 'L3*', 'L4*', 'L5*', 'L2*', 'L1*', 'L0*']),
    ('BP2 (C) J28', 1, ['L5*', 'L6*', 'L7*', 'L2*', 'L1*', 'L4*', 'L3*', 'L0*'], ['L7*', 'L6*', 'L5*', 'L4*', 'L3*', 'L2*', 'L1*', 'L0*']),
    ('BP3 (A) J27', 4, ['L7*', 'L2*', 'L1*', 'L0*', 'L5*', 'L6', 'L3*', 'L4'], ['L6', 'L5', 'L1', 'L4', 'L3*', 'L2', 'L7*', 'L0']),
    ('BP3 (A) J30', 2, ['L1', 'L2*', 'L3*', 'L4*', 'L5*', 'L6*', 'L7*', 'L0*'], ['L6*', 'L4*', 'L7*', 'L2*', 'L5*', 'L0*', 'L3*', 'L1*']),
    ('BP4 (A) J31', 25, ['L0*', 'L1*', 'L2*', 'L3*', 'L6*', 'L5*', 'L4*', 'L7*'], ['L0*', 'L1*', 'L4*', 'L3*', 'L6*', 'L5*', 'L2*', 'L7*']),
    ('BP4 (C) J34', 27, ['L6', 'L1', 'L0', 'L5*', 'L2', 'L3', 'L4*', 'L7'], ['L0', 'L3', 'L1', 'L5', 'L2', 'L6', 'L4', 'L7']),
    ('BP5 (A) J33', 26, ['L4*', 'L1*', 'L0*', 'L3*', 'L2*', 'L5*', 'L6*', 'L7*'], ['L0*', 'L3*', 'L2*', 'L5*', 'L1*', 'L6*', 'L4*', 'L7*']),
    ('BP5 (C) J36', 28, ['L4', 'L3', 'L0*', 'L1*', 'L6*', 'L7*', 'L2', 'L5*'], ['L0', 'L1*', 'L2*', 'L3*', 'L6', 'L5*', 'L4*', 'L7*']),
    ('BP6 (A) J39', 30, ['L2', 'L1', 'L4*', 'L5', 'L6*', 'L3', 'L0*', 'L7*'], ['L1*', 'L3*', 'L0*', 'L2*', 'L4*', 'L7*', 'L6*', 'L5*']),
    ('BP6 (C) J42', 32, ['L2*', 'L7*', 'L4*', 'L1*', 'L0*', 'L3*', 'L6*', 'L5*'], ['L0*', 'L2*', 'L6*', 'L1*', 'L5*', 'L3*', 'L4*', 'L7*']),
    ('BP7 (A) J37', 29, ['L5*', 'L1*', 'L3*', 'L4*', 'L2', 'L7*', 'L0', 'L6*'], ['L0*', 'L2*', 'L1*', 'L5*', 'L3*', 'L4*', 'L6*', 'L7*']),
    ('BP7 (C) J40', 31, ['L6*', 'L5*', 'L4*', 'L1*', 'L2*', 'L0*', 'L7*', 'L3*'], ['L1*', 'L0*', 'L2*', 'L3*', 'L5*', 'L6*', 'L4*', 'L7*']),
    ('QSFP0/1 J60', 24, ['L6', 'L5', 'L4', 'L3', 'L2', 'L7', 'L0', 'L1'], ['L4', 'L7', 'L6', 'L5', 'L0', 'L1', 'L2', 'L3']),
    ('QSFP2/3 J58', 22, ['L6', 'L3', 'L1', 'L5', 'L0', 'L4', 'L2', 'L7'], ['L4', 'L7', 'L6', 'L5*', 'L0', 'L1', 'L2', 'L3']),
    ('QSFP4/5 J56', 20, ['L6', 'L3', 'L2', 'L7', 'L4', 'L1', 'L0*', 'L5'], ['L3', 'L4*', 'L7', 'L6*', 'L0', 'L2*', 'L1', 'L5*']),
    ('QSFP6/7 J54', 18, ['L4', 'L3', 'L6', 'L5', 'L0', 'L1', 'L2', 'L7'], ['L6', 'L5', 'L7', 'L1', 'L2', 'L3', 'L4', 'L0']),
    ('QSFP8/9 J45', 9, ['L3', 'L6', 'L1', 'L2', 'L7', 'L4', 'L5', 'L0'], ['L3', 'L4', 'L1', 'L0', 'L7', 'L6', 'L5', 'L2']),
    ('QSFP10/11 J47', 11, ['L2', 'L5', 'L0', 'L4', 'L1', 'L7', 'L6', 'L3'], ['L2', 'L4', 'L1', 'L0', 'L5', 'L7', 'L3', 'L6']),
    ('QSFP12/13 J49', 13, ['L4', 'L7*', 'L0*', 'L2*', 'L3', 'L5*', 'L1*', 'L6'], ['L2', 'L3', 'L1', 'L0', 'L6', 'L4', 'L5', 'L7']),
    ('QSFP14/15 J51', 15, ['L2', 'L3', 'L0', 'L7*', 'L5', 'L4', 'L1', 'L6*'], ['L1', 'L5', 'L0', 'L2', 'L6', 'L7', 'L3', 'L4']),
    ('QSFP16/17 J59', 23, ['L0', 'L4', 'L2*', 'L7', 'L6', 'L3', 'L1', 'L5'], ['L0', 'L3', 'L2', 'L1', 'L6', 'L5', 'L7', 'L4']),
    ('QSFP18/19 J57', 21, ['L0', 'L1', 'L4', 'L3', 'L2', 'L5', 'L6', 'L7'], ['L0', 'L5', 'L2', 'L3*', 'L4', 'L1', 'L6', 'L7']),
    ('QSFP20/21 J55', 19, ['L3', 'L1', 'L7', 'L5', 'L4', 'L0', 'L6', 'L2'], ['L7', 'L1', 'L2', 'L3', 'L5', 'L0', 'L6*', 'L4']),
    ('QSFP22/23 J53', 17, ['L1', 'L3', 'L2', 'L0', 'L6', 'L5', 'L7', 'L4'], ['L2', 'L0', 'L4', 'L1', 'L5', 'L3', 'L6', 'L7']),
    ('QSFP24/25 J46', 10, ['L7', 'L0', 'L3', 'L2', 'L5', 'L4', 'L1', 'L6*'], ['L6', 'L7', 'L3', 'L4', 'L1', 'L5', 'L0', 'L2']),
    ('QSFP26/27 J48', 12, ['L5', 'L7*', 'L1', 'L2*', 'L6', 'L4*', 'L3', 'L0*'], ['L5', 'L7', 'L0', 'L4', 'L3', 'L6', 'L1', 'L2']),
    ('QSFP28/29 J50', 14, ['L5', 'L7', 'L1', 'L3', 'L4', 'L6', 'L0', 'L2'], ['L5', 'L6', 'L7', 'L4', 'L2', 'L0', 'L3', 'L1*']),
    ('QSFP30/31 J52', 16, ['L3', 'L6', 'L2', 'L4', 'L7', 'L5', 'L1', 'L0'], ['L7', 'L0', 'L3', 'L6', 'L5', 'L2', 'L1', 'L4']),
]

lane_map_rev_b = [
    ('BP0 (A) J21', 8, ['L0*', 'L2*', 'L3', 'L1', 'L7*', 'L6*', 'L5*', 'L4*'], ['L1*', 'L7*', 'L5*', 'L3*', 'L6*', 'L4*', 'L2*', 'L0*']),
    ('BP0 (C) J24', 6, ['L4', 'L3', 'L6*', 'L7*', 'L5*', 'L1', 'L2*', 'L0*'], ['L5', 'L6*', 'L7', 'L4*', 'L1', 'L0*', 'L2*', 'L3*']),
    ('BP1 (A) J19', 7, ['L4', 'L2', 'L6*', 'L0', 'L7', 'L3', 'L5', 'L1*'], ['L3', 'L7*', 'L6', 'L4*', 'L5', 'L2*', 'L1*', 'L0*']),
    ('BP1 (C) J22', 5, ['L7*', 'L4', 'L5*', 'L0*', 'L1*', 'L6*', 'L3*', 'L2*'], ['L5*', 'L6*', 'L7*', 'L2*', 'L1*', 'L0*', 'L4*', 'L3']),
    ('BP2 (A) J25', 3, ['L4', 'L2*', 'L5*', 'L6', 'L7', 'L0*', 'L3', 'L1*'], ['L7', 'L6', 'L3*', 'L4*', 'L5*', 'L2*', 'L1*', 'L0*']),
    ('BP2 (C) J28', 1, ['L5*', 'L6*', 'L7*', 'L2*', 'L1*', 'L4*', 'L3*', 'L0*'], ['L7*', 'L6*', 'L5*', 'L4*', 'L3*', 'L2*', 'L1*', 'L0*']),
    ('BP3 (A) J27', 4, ['L7*', 'L2*', 'L1*', 'L0*', 'L5*', 'L6', 'L3*', 'L4'], ['L6', 'L5', 'L1', 'L4', 'L3*', 'L2', 'L7*', 'L0']),
    ('BP3 (A) J30', 2, ['L1', 'L2*', 'L3*', 'L4*', 'L5*', 'L6*', 'L7*', 'L0*'], ['L6*', 'L4*', 'L7*', 'L2*', 'L5*', 'L0*', 'L3*', 'L1*']),
    ('BP4 (A) J31', 25, ['L0*', 'L1*', 'L2*', 'L3*', 'L6*', 'L5*', 'L4*', 'L7*'], ['L0*', 'L1*', 'L4*', 'L3*', 'L6*', 'L5*', 'L2*', 'L7*']),
    ('BP4 (C) J34', 27, ['L6', 'L1', 'L0', 'L5*', 'L2', 'L3', 'L4*', 'L7'], ['L0', 'L3', 'L1', 'L5', 'L2', 'L6', 'L4', 'L7']),
    ('BP5 (A) J33', 26, ['L4*', 'L1*', 'L0*', 'L3*', 'L2*', 'L5*', 'L6*', 'L7*'], ['L0*', 'L3*', 'L2*', 'L5*', 'L1*', 'L6*', 'L4*', 'L7*']),
    ('BP5 (C) J36', 28, ['L4', 'L3', 'L0*', 'L1*', 'L6*', 'L7*', 'L2', 'L5*'], ['L0', 'L1*', 'L2*', 'L3*', 'L6', 'L5*', 'L4*', 'L7*']),
    ('BP6 (A) J39', 30, ['L2', 'L1', 'L4*', 'L5', 'L6*', 'L3', 'L0*', 'L7*'], ['L1*', 'L3*', 'L0*', 'L2*', 'L4*', 'L7*', 'L6*', 'L5*']),
    ('BP6 (C) J42', 32, ['L2*', 'L7*', 'L4*', 'L1*', 'L0*', 'L3*', 'L6*', 'L5*'], ['L0*', 'L2*', 'L6*', 'L1*', 'L5*', 'L3*', 'L4*', 'L7*']),
    ('BP7 (A) J37', 29, ['L5*', 'L1*', 'L3*', 'L4*', 'L2', 'L7*', 'L0', 'L6*'], ['L0*', 'L2*', 'L1*', 'L5*', 'L3*', 'L4*', 'L6*', 'L7*']),
    ('BP7 (C) J40', 31, ['L6*', 'L5*', 'L4*', 'L1*', 'L2*', 'L0*', 'L7*', 'L3*'], ['L1*', 'L0*', 'L2*', 'L3*', 'L5*', 'L6*', 'L4*', 'L7*']),
    ('QSFP0/1 J60', 24, ['L3*', 'L4*', 'L5*', 'L6*', 'L1*', 'L0*', 'L7*', 'L2*'], ['L5*', 'L6*', 'L7*', 'L4*', 'L3*', 'L2*', 'L1*', 'L0*']),
    ('QSFP2/3 J58', 22, ['L5*', 'L1*', 'L3*', 'L6*', 'L7*', 'L2*', 'L4*', 'L0*'], ['L5', 'L6*', 'L7*', 'L4*', 'L3*', 'L2*', 'L1*', 'L0*']),
    ('QSFP4/5 J56', 20, ['L7*', 'L2*', 'L3*', 'L6*', 'L5*', 'L0', 'L1*', 'L4*'], ['L6', 'L7*', 'L4', 'L3*', 'L5', 'L1*', 'L2', 'L0*']),
    ('QSFP6/7 J54', 18, ['L5*', 'L6*', 'L3*', 'L4*', 'L7*', 'L2*', 'L1*', 'L0*'], ['L1*', 'L7*', 'L5*', 'L6*', 'L0*', 'L4*', 'L3*', 'L2*']),
    ('QSFP8/9 J45', 9, ['L2*', 'L1*', 'L6*', 'L3*', 'L0*', 'L5*', 'L4*', 'L7*'], ['L0*', 'L1*', 'L4*', 'L3*', 'L2*', 'L5*', 'L6*', 'L7*']),
    ('QSFP10/11 J47', 11, ['L4*', 'L0*', 'L5*', 'L2*', 'L3*', 'L6*', 'L7*', 'L1*'], ['L0*', 'L1*', 'L4*', 'L2*', 'L6*', 'L3*', 'L7*', 'L5*']),
    ('QSFP12/13 J49', 13, ['L2', 'L0', 'L7', 'L4*', 'L6*', 'L1', 'L5', 'L3*'], ['L0*', 'L1*', 'L3*', 'L2*', 'L7*', 'L5*', 'L4*', 'L6*']),
    ('QSFP14/15 J51', 15, ['L7', 'L0*', 'L3*', 'L2*', 'L6', 'L1*', 'L4*', 'L5*'], ['L2*', 'L0*', 'L5*', 'L1*', 'L4*', 'L3*', 'L7*', 'L6*']),
    ('QSFP16/17 J59', 23, ['L7*', 'L2', 'L4*', 'L0*', 'L5*', 'L1*', 'L3*', 'L6*'], ['L1*', 'L2*', 'L3*', 'L0*', 'L4*', 'L7*', 'L5*', 'L6*']),
    ('QSFP18/19 J57', 21, ['L3*', 'L4*', 'L1*', 'L0*', 'L7*', 'L6*', 'L5*', 'L2*'], ['L3', 'L2*', 'L5*', 'L0*', 'L7*', 'L6*', 'L1*', 'L4*']),
    ('QSFP20/21 J55', 19, ['L5*', 'L7*', 'L1*', 'L3*', 'L2*', 'L6*', 'L0*', 'L4*'], ['L3*', 'L2*', 'L1*', 'L7*', 'L4*', 'L6', 'L0*', 'L5*']),
    ('QSFP22/23 J53', 17, ['L0*', 'L2*', 'L3*', 'L1*', 'L4*', 'L7*', 'L5*', 'L6*'], ['L1*', 'L4*', 'L0*', 'L2*', 'L7*', 'L6*', 'L3*', 'L5*']),
    ('QSFP24/25 J46', 10, ['L2*', 'L3*', 'L0*', 'L7*', 'L6', 'L1*', 'L4*', 'L5*'], ['L4*', 'L3*', 'L7*', 'L6*', 'L2*', 'L0*', 'L5*', 'L1*']),
    ('QSFP26/27 J48', 12, ['L2', 'L1*', 'L7', 'L5*', 'L0', 'L3*', 'L4', 'L6*'], ['L4*', 'L0*', 'L7*', 'L5*', 'L2*', 'L1*', 'L6*', 'L3*']),
    ('QSFP28/29 J50', 14, ['L3*', 'L1*', 'L7*', 'L5*', 'L2*', 'L0*', 'L6*', 'L4*'], ['L4*', 'L7*', 'L6*', 'L5*', 'L1', 'L3*', 'L0*', 'L2*']),
    ('QSFP30/31 J52', 16, ['L4*', 'L2*', 'L6*', 'L3*', 'L0*', 'L1*', 'L5*', 'L7*'], ['L6*', 'L3*', 'L0*', 'L7*', 'L4*', 'L1*', 'L2*', 'L5*']),
]

if sidecar_rev == "A":
    lane_map = lane_map_rev_a
elif sidecar_rev == "B":
    lane_map = lane_map_rev_b
else:
    print('Unknown sidecar revision:', sidecar_rev)
    exit (1)

lane_re = re.compile('^L(\d)(\*)?')
mac_set = []

print('# AUTOGENERATED FILE! DO NOT EDIT! Use gen_port_map.py if changes are need.')
print('#')
print('# Board map for sidecar rev', sidecar_rev)
print('#')
print(
    '# connector kind',
    'connector',
    'channel',
    'mac_block',
    'mac_channel',
    'tx_lane',
    'tx_swap',
    'rx_lane',
    'rx_swap',
    'tx_eq_pre2',
    'tx_eq_pre1',
    'tx_eq_main',
    'tx_eq_post1',
    'tx_eq_post2',
    sep=', '
)

connector = 1

# Captures the transmitter equalization parameters.
EqParameters = namedtuple(
    "EqParameters",
    ["pre2", "pre1", "main", "post1", "post2"]
)

def connector_tx_eq_parameters(kind: ConnectorKind, index: int) -> EqParameters:
    """Return the equalization parameters for a connector on a given channel.

    At this point, we return the same thing for all connectors, but we may
    feasibly wish to tweak things based on cabling information and measurements.

    Parameters
    ---------
    kind: ConnectorKind
        The kind of connector whose gains are returned.
    index: int
        The index of the connector in the entire map.
    """
    return EqParameters(pre2=0, pre1=0, main=26, post1=0, post2=0)

for lane_group, (comment, mac_block, tx_lanes, rx_lanes) in enumerate(lane_map):
    print('#', comment)

    # Split a lane group into two slices of four lanes to allocate to two
    # connectors.
    logical_lanes = iter(zip(tx_lanes, rx_lanes))
    mac_channel = 0

    while logical_lane_slice := list(itertools.islice(logical_lanes, 4)):
        for connector_channel, (tx_lane, rx_lane) in enumerate(logical_lane_slice):
            if comment.startswith("BP"):
                connector_kind = ConnectorKind.BACKPLANE
            elif comment.startswith("QSFP"):
                connector_kind = ConnectorKind.FRONT_IO
            eq = connector_tx_eq_parameters(connector_kind, lane_group)

            tx_match = lane_re.match(tx_lane)
            assert tx_match, 'expected TX match'
            tx_polarity = (1 if tx_match.group(2) == '*' else 0)

            rx_match = lane_re.match(rx_lane)
            assert rx_match, 'expected RX match'
            rx_polarity = (1 if rx_match.group(2) == '*' else 0)

            print(
                connector_kind.value,
                connector,
                connector_channel,
                mac_block,
                mac_channel,
                tx_match.group(1),
                tx_polarity,
                rx_match.group(1),
                rx_polarity,
                *eq,
                sep=", "
            )

            mac_channel = mac_channel + 1

        print()
        connector = connector + 1

    mac_set.append(mac_block)

# Add the CPU port, connecting to the VSC7448
print("""# VSC7448 (lane 0 only)
{connector_kind}, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0, 25, -5, 0
{connector_kind}, 65, 1, 0, 1, 1, 0, 1, 0, 0, 0, 25, -5, 0
{connector_kind}, 65, 2, 0, 2, 2, 0, 2, 0, 0, 0, 25, -5, 0
{connector_kind}, 65, 3, 0, 3, 3, 0, 3, 0, 0, 0, 25, -5, 0
""".format(connector_kind=ConnectorKind.CPU.value))

assert sorted(mac_set) == list(range(1, 33)), \
    "One or more MAC blocks missing from port map"
