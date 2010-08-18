#!/usr/bin/python

# Copyright (C) 2010, Nokia (ivan.frade@nokia.com)
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
#
"""
Write values in tracker and check the actual values are written
on the files. Note that these tests are highly platform dependant.
"""
import sys, os, dbus
import time
import shutil

from common.utils.system import TrackerSystemAbstraction
from common.utils.helpers import StoreHelper, ExtractorHelper
from common.utils import configuration as cfg
import unittest2 as ut
from common.utils.expectedFailure import expectedFailureBug

BASEDIR = os.environ['HOME']
REASONABLE_TIMEOUT = 5 # Seconds we wait for tracker-writeback to do the work

def uri (filename):
    return "file://" + os.path.join (BASEDIR, filename)


class CommonTrackerWritebackTest (ut.TestCase):
    """
    Superclass to share methods. Shouldn't be run by itself.
    """
	     
    @classmethod
    def __prepare_directories (self):
        #
        #     ~/test-writeback-monitored/
        #     ~/test-writeback-no-monitored/
        #
        
        for d in ["test-writeback-monitored",
                  "test-writeback-no-monitored"]:
            directory = os.path.join (BASEDIR, d)
            if (os.path.exists (directory)):
                shutil.rmtree (directory)
            os.makedirs (directory)


        if (os.path.exists (os.getcwd() + "/test-writeback-data")):
            # Use local directory if available
            datadir = os.getcwd() + "/test-writeback-data"
        else:
            datadir = os.path.join (cfg.DATADIR, "tracker-tests",
                                    "test-writeback-data")

        for root, dirs, testfile in os.walk (datadir):

            def is_valid_file (f):
                return not (tf.endswith ("~") or tf.startswith ("Makefile"))

            valid_files = [os.path.join (root, tf) for tf in testfile if is_valid_file (tf)]
            for f in valid_files:
                print "Copying", f, os.path.join (BASEDIR, "test-writeback-monitored")
                shutil.copy (f, os.path.join (BASEDIR, "test-writeback-monitored"))

    
    @classmethod 
    def setUpClass (self):
        #print "Starting the daemon in test mode"
        self.__prepare_directories ()
        
        self.system = TrackerSystemAbstraction ()

        if (os.path.exists (os.getcwd() + "/test-configurations/writeback")):
            # Use local directory if available
            confdir = os.getcwd() + "/test-configurations/writeback"
        else:
            confdir = os.path.join (cfg.DATADIR, "tracker-tests",
                                    "test-configurations", "writeback")
        self.system.tracker_writeback_testing_start (confdir)
        # Returns when ready
        print "Ready to go!"
        
    @classmethod
    def tearDownClass (self):
        #print "Stopping the daemon in test mode (Doing nothing now)"
        self.system.tracker_writeback_testing_stop ()
    

class WritebackMonitoredTest (CommonTrackerWritebackTest):
    """
    Write in tracker store the properties witih writeback support and check
    that the new values are actually in the file
    """
    def setUp (self):
        self.tracker = StoreHelper ()
        self.extractor = ExtractorHelper ()

    def tearDown (self):
        # Give it more time between tests to avoid random failures?
        pass

    def __clean_property (self, property_name, fileuri, expectFailure=True):
        """
        Remove the property for the fileuri (file://...)
        """
        CLEAN = """
           DELETE { ?u %s ?whatever }
           WHERE  {
               ?u nie:url '%s' ;
                  %s ?whatever .
           
           }
        """
        try:
            self.tracker.update (CLEAN % (property_name, fileuri, property_name))
        except Exception, e:
            print e
            assert expectFailure
                                

    def __writeback_test (self, filename, mimetype, prop, expectedKey=None):
        """
        Set a value in @prop for the @filename. Then ask tracker-extractor
        for metadata and check in the results dictionary if the property is there.

        Note: given the special translation of some property-names in the dictionary
        with extracted metadata, there is an optional parameter @expectedKey
        to specify what property to check in the dictionary. If None, then
        the @prop is used.
        """

        TEST_VALUE = prop.replace (":","") + "test"
        SPARQL_TMPL = """
           INSERT { ?u %s '%s' }
           WHERE  { ?u nie:url '%s' }
        """ 
        self.__clean_property (prop, uri(filename))
        self.tracker.update (SPARQL_TMPL % (prop, TEST_VALUE, uri(filename)))
        
        # There is no way to know when the operation is finished
        time.sleep (REASONABLE_TIMEOUT)
        
        results = self.extractor.get_metadata (uri (filename), mimetype)
        keyDict = expectedKey or prop
        self.assertEquals (results[keyDict][0], TEST_VALUE)
        self.__clean_property (prop, uri(filename), False)


    def __writeback_hasTag_test (self, filename, mimetype):

        SPARQL_TMPL = """
            INSERT {
              <test://writeback-hasTag-test/1> a nao:Tag ;
                        nao:prefLabel "testTag" .

              ?u nao:hasTag <test://writeback-hasTag-test/1> .
            } WHERE {
              ?u nie:url '%s' .
            }
        """

        CLEAN_VALUE = """
           DELETE {
              <test://writeback-hasTag-test/1> a rdfs:Resource.
              ?u nao:hasTag <test://writeback-hasTag-test/1> .
           } WHERE {
              ?u nao:hasTag <test://writeback-hasTag-test/1> .
           }
        """

        self.tracker.update (SPARQL_TMPL % (uri (filename)))

        time.sleep (REASONABLE_TIMEOUT)

        results = self.extractor.get_metadata (uri (filename), mimetype)
        self.assertIn ("testTag", results ["nao:hasTag:prefLabel"])


    # JPEG test
    def test_001_jpeg_title (self):
        FILENAME = "test-writeback-monitored/writeback-test-1.jpeg"
        self.__writeback_test (FILENAME, "image/jpeg", "nie:title")

    def test_002_jpeg_description (self):
        FILENAME = "test-writeback-monitored/writeback-test-1.jpeg"
        self.__writeback_test (FILENAME, "image/jpeg", "nie:description")

    def test_003_jpeg_keyword (self):
        FILENAME = "test-writeback-monitored/writeback-test-1.jpeg"
        self.__writeback_test (FILENAME, "image/jpeg", "nie:keyword", "nao:hasTag:prefLabel")

    def test_004_jpeg_hasTag (self):
        FILENAME = "test-writeback-monitored/writeback-test-1.jpeg"
        self.__writeback_hasTag_test (FILENAME, "image/jpeg")

        
    # TIFF tests
    def test_011_tiff_title (self):
        FILENAME = "test-writeback-monitored/writeback-test-2.tif"
        self.__writeback_test (FILENAME, "image/tiff", "nie:title")

    def test_012_tiff_description (self):
        FILENAME = "test-writeback-monitored/writeback-test-2.tif"
        self.__writeback_test (FILENAME, "image/tiff", "nie:description")
        
    def test_013_tiff_keyword (self):
        FILENAME = "test-writeback-monitored/writeback-test-2.tif"
        self.__writeback_test (FILENAME, "image/tiff", "nie:keyword", "nao:hasTag:prefLabel")

    def test_014_tiff_hasTag (self):
        FILENAME = "test-writeback-monitored/writeback-test-2.tif"
        self.__writeback_hasTag_test (FILENAME, "image/tiff")
      
        

    # PNG tests
    ## @expectedFailureBug ("NB#185070")
    def test_021_png_title (self):
        FILENAME = "test-writeback-monitored/writeback-test-4.png"
        self.__writeback_test (FILENAME, "image/png", "nie:title")

    @expectedFailureBug ("NB#185070")
    def test_022_png_description (self):
        FILENAME = "test-writeback-monitored/writeback-test-4.png"
        self.__writeback_test (FILENAME, "image/png", "nie:description")
        
    @expectedFailureBug ("NB#185070")
    def test_023_png_keyword (self):
        FILENAME = "test-writeback-monitored/writeback-test-4.png"
        self.__writeback_test (FILENAME, "image/png", "nie:keyword", "nao:hasTag:prefLabel")

    @expectedFailureBug("NB#185070")
    def test_024_png_hasTag (self):
        FILENAME = "test-writeback-monitored/writeback-test-4.png"
        self.__writeback_hasTag_test (FILENAME, "image/png")

if __name__ == "__main__":
    ut.main ()
