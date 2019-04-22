#!/usr/bin/env python3
#
#    This file is part of Leela Zero.
#    Copyright (C) 2017-2018 Gian-Carlo Pascutto
#
#    Leela Zero is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    Leela Zero is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.

import os
import sys
import glob
import gzip
import random
import math
import multiprocessing as mp
import numpy as np
import time
import tensorflow as tf
import signal
from tfprocess import TFProcess

# 16 planes, 1 side to move, 1 x 82 probs, 1 winner = 19 lines
DATA_ITEM_LINES = 16 + 1 + 1 + 1

# Sane values are from 4096 to 64 or so. The maximum depends on the amount
# of RAM in your GPU and the network size. You need to adjust the learning rate
# if you change this.
BATCH_SIZE = 128
BOARD_SIZE = 13
TOTAL_CROSSING = BOARD_SIZE*BOARD_SIZE

def remap_vertex(vertex, symmetry):
    """
        Remap a go board coordinate according to a symmetry.
    """
    assert vertex >= 0 and vertex < TOTAL_CROSSING
    x = vertex % BOARD_SIZE
    y = vertex // BOARD_SIZE
    if symmetry >= 4:
        x, y = y, x
        symmetry -= 4
    if symmetry == 1 or symmetry == 3:
        x = BOARD_SIZE - x - 1
    if symmetry == 2 or symmetry == 3:
        y = BOARD_SIZE - y - 1
    return y * BOARD_SIZE + x

class ChunkParser:
    def __init__(self, chunks, workers=None):
        # Build probility reflection tables. The last element is 'pass' and is identity mapped.
        self.prob_reflection_table = [[remap_vertex(vertex, sym) for vertex in range(TOTAL_CROSSING)]+[TOTAL_CROSSING] for sym in range(8)]
        # Build full 16-plane reflection tables.
        self.full_reflection_table = [
            [remap_vertex(vertex, sym) + p * TOTAL_CROSSING for p in range(16) for vertex in range(TOTAL_CROSSING) ]
            for sym in range(8) ]
        # Convert both to np.array. This avoids a conversion step when they're actually used.
        self.prob_reflection_table = [ np.array(x, dtype=np.int64) for x in self.prob_reflection_table ]
        self.full_reflection_table = [ np.array(x, dtype=np.int64) for x in self.full_reflection_table ]
        # Build the all-zeros and all-ones flat planes, used for color-to-move.
        self.flat_planes = [ b'\0' * TOTAL_CROSSING, b'\1' * TOTAL_CROSSING ]

        # Start worker processes, leave 2 for TensorFlow
        if workers is None:
            workers = max(1, mp.cpu_count() - 2)
        print("Using {} worker processes.".format(workers))
        self.readers = []
        self.mp_instances = []
        for _ in range(workers):
            read, write = mp.Pipe(False)
            mp_instance = mp.Process(target=self.task, args=(chunks, write))
            self.mp_instances.append(mp_instance)
            mp_instance.start()
            self.readers.append(read)

    def convert_train_data(self, text_item, symmetry):
        """
            Convert textual training data to a tf.train.Example

            Converts a set of 9 lines of text into a pythonic dataformat.
            [[plane_1],[plane_2],...],...
            [probabilities],...
            winner,...
        """
        assert symmetry >= 0 and symmetry < 8

        # We start by building a list of 16 planes, each being a 9*9 == 81 element array
        # of type np.uint8
        planes = []
        for plane in range(0, 16):
            # first 80 first bits are 20 hex chars, encoded MSB
            hex_string = text_item[plane][0:42]
            array = np.unpackbits(np.frombuffer(bytearray.fromhex(hex_string), dtype=np.uint8))
            # Remaining bit that didn't fit. Encoded LSB so
            # it needs to be specially handled.
            last_digit = text_item[plane][42]
            assert last_digit == "0" or last_digit == "1"
            # Apply symmetry and append
            planes.append(array)
            planes.append(np.array([last_digit], dtype=np.uint8))

        # We flatten to a single array of len 16*9*9, type=np.uint8
        planes = np.concatenate(planes)

        # We use the full length reflection tables to apply symmetry
        # to all 16 planes simultaneously
        planes = planes[self.full_reflection_table[symmetry]]
        # Convert the array to a byte string
        planes = [ planes.tobytes() ]

        # Now we add the two final planes, being the 'color to move' planes.
        # These already a fully symmetric, so we add them directly as byte
        # strings of length 81.
        stm = text_item[16][0]
        assert stm == "0" or stm == "1"
        stm = int(stm)
        planes.append(self.flat_planes[1 - stm])
        planes.append(self.flat_planes[stm])

        # Flatten all planes to a single byte string
        planes = b''.join(planes)
        assert len(planes) == (18 * TOTAL_CROSSING)

        # Load the probabilities.
        probabilities = np.array(text_item[17].split()).astype(float)
        if np.any(np.isnan(probabilities)):
            # Work around a bug in leela-zero v0.3, skipping any
            # positions that have a NaN in the probabilities list.
            return False, None
        # Apply symmetries to the probabilities.
        probabilities = probabilities[self.prob_reflection_table[symmetry]]
        assert len(probabilities) == TOTAL_CROSSING+1

        # Load the game winner color.
        winner = float(text_item[18])
        assert winner == 1.0 or winner == -1.0

        # Construct the Example protobuf
        example = tf.train.Example(features=tf.train.Features(feature={
            'planes' : tf.train.Feature(bytes_list=tf.train.BytesList(value=[planes])),
            'probs' : tf.train.Feature(float_list=tf.train.FloatList(value=probabilities)),
            'winner' : tf.train.Feature(float_list=tf.train.FloatList(value=[winner]))}))
        return True, example.SerializeToString()

    def task(self, chunks, writer):
        while True:
            random.shuffle(chunks)
            for chunk in chunks:
                with gzip.open(chunk, 'r') as chunk_file:
                    file_content = chunk_file.read().splitlines()
                    item_count = len(file_content) // DATA_ITEM_LINES
                    # Pick only 1 in every 16 positions
                    picked_items = random.sample(range(item_count),
                                                 (item_count + 15) // 16)
                    for item_idx in picked_items:
                        pick_offset = item_idx * DATA_ITEM_LINES
                        item = file_content[pick_offset:pick_offset + DATA_ITEM_LINES]
                        str_items = [str(line, 'ascii') for line in item]
                        # Pick a random symmetry to apply
                        symmetry = random.randrange(8)
                        success, data = self.convert_train_data(str_items, symmetry)
                        if success:
                            # Send it down the pipe.
                            writer.send_bytes(data)

    def parse_chunk(self):
        while True:
            for r in self.readers:
                yield r.recv_bytes();

def get_chunks(data_prefix):
    x = glob.glob(data_prefix + "*.gz")
    x.sort()
    x = x[int(len(x)*0.8):]

    return x

#
# Tests to check that records can round-trip successfully
def generate_fake_pos():
    """
        Generate a random game position.
        Result is ([[81] * 18], [82], [1])
    """
    # 1. 18 binary planes of length 81
    planes = [np.random.randint(2, size=TOTAL_CROSSING).tolist() for plane in range(16)]
    stm = float(np.random.randint(2))
    planes.append([stm] * TOTAL_CROSSING)
    planes.append([1. - stm] * TOTAL_CROSSING)
    # 2. 82 probs
    probs = np.random.randint(3, size=TOTAL_CROSSING+1).tolist()
    # 3. And a winner: 1 or -1
    winner = [ 2 * float(np.random.randint(2)) - 1 ]
    return (planes, probs, winner)

def run_test(parser):
    """
        Test game position decoding.
    """

    # First, build a random game position.
    planes, probs, winner = generate_fake_pos()

    # Convert that to a text record in the same format
    # generated by dump_supervised
    items = []
    for p in range(16):
        # generate first 360 bits
        h = np.packbits([int(x) for x in planes[p][0:360]]).tobytes().hex()
        # then add the stray single bit
        h += str(planes[p][360]) + "\n"
        items.append(h)
    # then who to move
    items.append(str(int(planes[17][0])) + "\n")
    # then probs
    items.append(' '.join([str(x) for x in probs]) + "\n")
    # and finally a winner
    items.append(str(int(winner[0])) + "\n")

    # Have an input string. Running it through parsing to see
    # if it gives the same result we started with.
    # We need a tf.Session() as we're going to use the tensorflow
    # decoding framework for part of the parsing.
    with tf.Session() as sess:
        # We apply and check every symmetry.
        for symmetry in range(8):
            result = parser.convert_train_data(items, symmetry)
            assert result[0] == True
            # We got back a serialized tf.train.Example, which we need to decode.
            graph = _parse_function(result[1])
            data = sess.run(graph)
            data = (data[0].tolist(), data[1].tolist(), data[2].tolist())

            # Apply the symmetry to the original
            sym_planes = [ [ plane[remap_vertex(vertex, symmetry)] for vertex in range(TOTAL_CROSSING) ] for plane in planes ]
            sym_probs = [ probs[remap_vertex(vertex, symmetry)] for vertex in range(TOTAL_CROSSING)] + [probs[TOTAL_CROSSING]]

            # Check that what we got out matches what we put in.
            assert data == (sym_planes, sym_probs, winner)
    print("Test parse passes")


# Convert a tf.train.Example protobuf into a tuple of tensors
# NB: This conversion is done in the tensorflow graph, NOT in python.
def _parse_function(example_proto):
    features = {"planes": tf.FixedLenFeature((1), tf.string),
                "probs": tf.FixedLenFeature((TOTAL_CROSSING+1), tf.float32),
                "winner": tf.FixedLenFeature((1), tf.float32)}
    parsed_features = tf.parse_single_example(example_proto, features)
    # We receives the planes as a byte array, but we really want
    # floats of shape (18, 9*9), so decode, cast, and reshape.
    planes = tf.decode_raw(parsed_features["planes"], tf.uint8)
    planes = tf.to_float(planes)
    planes = tf.reshape(planes, (18, TOTAL_CROSSING))
    # the other features are already in the correct shape as return as-is.
    return planes, parsed_features["probs"], parsed_features["winner"]

def benchmark(parser):
    gen = parser.parse_chunk()
    while True:
        start = time.time()
        for _ in range(10000):
            next(gen)
        end = time.time()
        print("{} pos/sec {} secs".format( 10000. / (end - start), (end - start)))

def split_chunks(chunks, test_ratio):
    splitpoint = 1 + int(len(chunks) * (1.0 - test_ratio))
    return (chunks[:splitpoint], chunks[splitpoint:])

def main(args):
    train_data_prefix = args.pop(0)

    chunks = get_chunks(train_data_prefix)
    print("Found {0} chunks".format(len(chunks)))

    if not chunks:
        return

    # The following assumes positions from one game are not
    # spread through chunks.
    random.shuffle(chunks)
    training, test = split_chunks(chunks, 0.1)
    print("Training with {0} chunks, validating on {1} chunks".format(
        len(training), len(test)))

    #run_test(parser)
    #benchmark(parser)

    train_parser = ChunkParser(training)
    dataset = tf.data.Dataset.from_generator(
        train_parser.parse_chunk, output_types=(tf.string))
    dataset = dataset.shuffle(1 << 18)
    dataset = dataset.map(_parse_function)
    dataset = dataset.batch(BATCH_SIZE)
    dataset = dataset.prefetch(4)
    train_iterator = dataset.make_one_shot_iterator()

    test_parser = ChunkParser(test)
    dataset = tf.data.Dataset.from_generator(
        test_parser.parse_chunk, output_types=(tf.string))
    dataset = dataset.map(_parse_function)
    dataset = dataset.batch(BATCH_SIZE)
    dataset = dataset.prefetch(4)
    test_iterator = dataset.make_one_shot_iterator()

    tfprocess = TFProcess()
    tfprocess.init(dataset, train_iterator, test_iterator)
    if args:
        restore_file = args.pop(0)
        tfprocess.restore(restore_file)

    for _ in range(12001):
        tfprocess.process(BATCH_SIZE)

    for x in train_parser.mp_instances:
        x.terminate()
        x.join()

    os.killpg(0, signal.SIGTERM)

if __name__ == "__main__":
    main(sys.argv[1:])
    mp.freeze_support()
