#!/usr/bin/env python
from six import iteritems
import sys
import unittest
from array import array as pa  # Python array
import kmer
import numpy as np
import itertools
from download_genomes import xfirstline
from parse_nodes import generate_python_class_map


def load_nameidmap(path):
    with open(path) as f:
        return {el[0]: int(el[1]) for el in
                map(lambda x: x.split('\t'), f)}


class TaxEntry(object):
    def __init__(self, line, gi2taxmap, classlvl_map):
        toks = (i.strip() for i in line.split('|'))
        self.id = int(next(toks))
        self.parent = int(next(toks))
        try:
            self.depth = classlvl_map[next(toks)]
        except KeyError:
            print("'" + "', '".join(str(i) for i in classlvl_map.keys()) + "'")
            raise
        self.name = None
        self.genome_paths = []

    def update(self, path):
        self.genome_paths.append(path)

    def __hash__(self):
        '''Hash function for TaxEntry.
           Note: this assumes that the user does not re-use tax ids.'''
        return hash(self.id)

    def __eq__(self, other):
        if isinstance(self, type(other)):
            return self.id == other.id
        raise NotImplementedError("Cannot compare TaxEntry "
                                  "objects with other objects.")

    def __cmp__(self, other):
        if isinstance(self, type(other)):
            if self.depth < other.depth:
                return 1
            if self.depth > other.depth:
                return -1
            return 0
        raise NotImplementedError("TaxEntry cannot be compared "
                                  "to %s" % str(type(other)))


class Taxonomy(object):

    def __init__(self, gi2taxpath, nodespath):
        self.lvl_map = generate_python_class_map()
        try:
            assert "superkingdom" in self.lvl_map
        except AssertionError:
            print("Map string keys: " + ", ".join(i for i in self.lvl_map.keys() if isinstance(i, str)))
            print("Map int keys: " + ", ".join(str(i) for i in self.lvl_map.keys() if isinstance(i, int)))
            raise
        self.taxes = []
        with open(gi2taxpath) as f:
            self.gi2taxmap = {line.split()[0]: int(line.split()[1]) for
                              line in f}
        self.nodes_path = nodespath
        self.add_file(self.nodes_path)

    def add_file(self, path):
        with open(path) as f:
            for line in f:
                self.add_line(line)

    def get_parents(self, el):
        parent = self.__getitem__(el)
        ret = {parent}
        while parent:
            parent = self.__getitem__(el)
            ret.add(parent)
        return ret

    def build_child_ancestor_map():
        # This would take some careful thinking.
        raise NotImplementedError("Stuff")

    def __getitem__(self, el):
        try:
            return next(x for x in self.taxes if x == el or x.id == el)
        except StopIteration:
            raise KeyError("Missing element or id %s" % str(el))

    def __setitem__(self, key, val):
        try:
            next(x for x in self.taxes if x == el or x.id == el).genome_paths.append(val)
        except StopIteration:
            raise KeyError("Missing element or id %s" % str(el))

    def __contains__(self, key):
        return key in self.taxes or any(tax.id == key for tax in self.taxes)

    def add_line(self, line):
        self.taxes.append(TaxEntry(line, self.gi2taxmap, self.lvl_map))

    def add_genome_path(self, path, tax):
        try:
            assert isinstance(tax, int)
            next(x for x in self.taxes if
                 x.id == tax).genome_paths.append(path)
        except AssertionError:
            print("Assertion failed. Tax is not an int. Tax: %s, %s" %
                  (str(tax), repr(tax)))
            raise
        except StopIteration:
            print("Missing tax %i" % tax)
            raise


if __name__ == "__main__":
    NAMEID_MAP_PATH = "save/combined_nip.txt"
    NODES_PATH = "ref/nodes.dmp"

    class TC1(unittest.TestCase):

        def test_testing(self):
            pass

        def test_assert(self):
            self.assertTrue(True)

        def test_assert_false(self):
            self.assertFalse(False)

        def test_load_nameid(self):
            map = load_nameidmap(NAMEID_MAP_PATH)
            for k, v in iteritems(map):
                self.assertIsInstance(v, int)
                self.assertIsInstance(k, str)

        def test_taxonomy_build(self):
            NODES_PATH = "ref/nodes.dmp"
            tax = Taxonomy(NAMEID_MAP_PATH, NODES_PATH)
            self.assertTrue(0 in tax)

    unittest.main()
