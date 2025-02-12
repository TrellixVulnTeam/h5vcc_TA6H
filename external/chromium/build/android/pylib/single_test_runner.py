# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import glob
import logging
import os
import sys

import android_commands
from android_commands import errors
from base_test_runner import BaseTestRunner
import constants
import debug_info
import perf_tests_helper
import run_tests_helper
from test_package_apk import TestPackageApk
from test_package_executable import TestPackageExecutable
from test_result import BaseTestResult, TestResults


class SingleTestRunner(BaseTestRunner):
  """Single test suite attached to a single device.

  Args:
    device: Device to run the tests.
    test_suite: A specific test suite to run, empty to run all.
    gtest_filter: A gtest_filter flag.
    test_arguments: Additional arguments to pass to the test binary.
    timeout: Timeout for each test.
    cleanup_test_files: Whether or not to cleanup test files on device.
    tool: Name of the Valgrind tool.
    shard_index: index number of the shard on which the test suite will run.
    dump_debug_info: Whether or not to dump debug information.
    build_type: 'Release' or 'Debug'.
    in_webkit_checkout: Whether the suite is being run from a WebKit checkout.
  """

  def __init__(self, device, test_suite, gtest_filter, test_arguments, timeout,
               cleanup_test_files, tool_name, shard_index, dump_debug_info,
               fast_and_loose, build_type, in_webkit_checkout):
    BaseTestRunner.__init__(self, device, tool_name, shard_index, build_type)
    self._running_on_emulator = self.device.startswith('emulator')
    self._gtest_filter = gtest_filter
    self._test_arguments = test_arguments
    self.test_results = TestResults()
    if dump_debug_info:
      self.dump_debug_info = debug_info.GTestDebugInfo(
          self.adb, device,
          os.path.basename(test_suite), gtest_filter)
    else:
      self.dump_debug_info = None
    self.fast_and_loose = fast_and_loose
    self.in_webkit_checkout = in_webkit_checkout

    logging.warning('Test suite: ' + test_suite)
    if os.path.splitext(test_suite)[1] == '.apk':
      self.test_package = TestPackageApk(
          self.adb,
          device,
          test_suite,
          timeout,
          cleanup_test_files,
          self.tool,
          self.dump_debug_info)
    else:
      # Put a copy into the android out/target directory, to allow stack trace
      # generation.
      symbols_dir = os.path.join(constants.CHROME_DIR, 'out', build_type,
                                 'lib.target')
      self.test_package = TestPackageExecutable(
          self.adb,
          device,
          test_suite, timeout,
          cleanup_test_files,
          self.tool,
          self.dump_debug_info,
          symbols_dir)

  def _TestSuiteRequiresMockTestServer(self):
    """Returns True if the test suite requires mock test server."""
    tests_require_net_test_server = ['unit_tests', 'net_unittests',
                                     'content_unittests']
    return (self.test_package.test_suite_basename in
            tests_require_net_test_server)

  def _GetFilterFileName(self):
    """Returns the filename of gtest filter."""
    return os.path.join(
        sys.path[0], 'gtest_filter',
        self.test_package.test_suite_basename + '_disabled')

  def _GetAdditionalEmulatorFilterName(self):
    """Returns the filename of additional gtest filter for emulator."""
    return os.path.join(
        sys.path[0], 'gtest_filter',
        self.test_package.test_suite_basename +
        '_emulator_additional_disabled')

  def GetDisabledTests(self):
    """Returns a list of disabled tests.

    Returns:
      A list of disabled tests obtained from gtest_filter/test_suite_disabled.
    """
    disabled_tests = run_tests_helper.GetExpectations(self._GetFilterFileName())
    if self._running_on_emulator:
      # Append emulator's filter file.
      disabled_tests.extend(run_tests_helper.GetExpectations(
          self._GetAdditionalEmulatorFilterName()))
    return disabled_tests

  def GetDataFilesForTestSuite(self):
    """Returns a list of data files/dirs needed by the test suite."""
    # Ideally, we'd just push all test data. However, it has >100MB, and a lot
    # of the files are not relevant (some are used for browser_tests, others for
    # features not supported, etc..).
    if self.test_package.test_suite_basename in ['base_unittests',
                                                 'sql_unittests',
                                                 'unit_tests']:
      test_files = [
          'base/data/file_util_unittest',
          'base/data/json/bom_feff.json',
          'base/prefs/test/data/pref_service',
          'chrome/test/data/download-test1.lib',
          'chrome/test/data/extensions/bad_magic.crx',
          'chrome/test/data/extensions/good.crx',
          'chrome/test/data/extensions/icon1.png',
          'chrome/test/data/extensions/icon2.png',
          'chrome/test/data/extensions/icon3.png',
          'chrome/test/data/extensions/allow_silent_upgrade/',
          'chrome/test/data/extensions/app/',
          'chrome/test/data/extensions/bad/',
          'chrome/test/data/extensions/effective_host_permissions/',
          'chrome/test/data/extensions/empty_manifest/',
          'chrome/test/data/extensions/good/Extensions/',
          'chrome/test/data/extensions/manifest_tests/',
          'chrome/test/data/extensions/page_action/',
          'chrome/test/data/extensions/permissions/',
          'chrome/test/data/extensions/script_and_capture/',
          'chrome/test/data/extensions/unpacker/',
          'chrome/test/data/bookmarks/',
          'chrome/test/data/components/',
          'chrome/test/data/extensions/json_schema_test.js',
          'chrome/test/data/History/',
          'chrome/test/data/json_schema_validator/',
          'chrome/test/data/pref_service/',
          'chrome/test/data/serializer_nested_test.js',
          'chrome/test/data/serializer_test.js',
          'chrome/test/data/serializer_test_nowhitespace.js',
          'chrome/test/data/top_sites/',
          'chrome/test/data/web_app_info/',
          'chrome/test/data/web_database',
          'chrome/test/data/webui/',
          'chrome/test/data/zip',
          'chrome/third_party/mock4js/',
          'content/browser/gpu/software_rendering_list.json',
          'net/data/cache_tests/insert_load1',
          'net/data/cache_tests/dirty_entry5',
          'net/data/ssl/certificates/',
          'ui/base/test/data/data_pack_unittest',
      ]
      if self.test_package.test_suite_basename == 'unit_tests':
        test_files += ['chrome/test/data/simple_open_search.xml']
        # The following are spell check data. Now only list the data under
        # third_party/hunspell_dictionaries which are used by unit tests.
        old_cwd = os.getcwd()
        os.chdir(constants.CHROME_DIR)
        test_files += glob.glob('third_party/hunspell_dictionaries/*.bdic')
        os.chdir(old_cwd)
      return test_files
    elif self.test_package.test_suite_basename == 'media_unittests':
      return [
          'media/test/data',
      ]
    elif self.test_package.test_suite_basename == 'net_unittests':
      return [
          'chrome/test/data/animate1.gif',
          'chrome/test/data/simple.html',
          'net/data/cache_tests',
          'net/data/filter_unittests',
          'net/data/ftp',
          'net/data/proxy_resolver_v8_unittest',
          'net/data/ssl/certificates',
          'net/data/url_request_unittest/',
          'net/data/proxy_script_fetcher_unittest'
          ]
    elif self.test_package.test_suite_basename == 'ui_tests':
      return [
          'chrome/test/data/dromaeo',
          'chrome/test/data/json2.js',
          'chrome/test/data/sunspider',
          'chrome/test/data/v8_benchmark',
          'chrome/test/perf/v8_benchmark_uitest.js',
          ]
    elif self.test_package.test_suite_basename == 'content_unittests':
      return [
          'content/test/data/gpu/webgl_conformance_test_expectations.txt',
          'net/data/ssl/certificates/',
          'webkit/data/dom_storage/webcore_test_database.localstorage',
          'third_party/hyphen/hyph_en_US.dic',
          ]
    elif self.test_package.test_suite_basename == 'media_unittests':
      return [
          'media/test/data',
          ]
    return []

  def LaunchHelperToolsForTestSuite(self):
    """Launches helper tools for the test suite.

    Sometimes one test may need to run some helper tools first in order to
    successfully complete the test.
    """
    if self._TestSuiteRequiresMockTestServer():
      self.LaunchChromeTestServerSpawner()

  def StripAndCopyFiles(self):
    """Strips and copies the required data files for the test suite."""
    self.test_package.StripAndCopyExecutable()
    self.test_package.PushDataAndPakFiles()
    self.tool.CopyFiles()
    test_data = self.GetDataFilesForTestSuite()
    if test_data and not self.fast_and_loose:
      # Make sure SD card is ready.
      self.adb.WaitForSdCardReady(20)
      for data in test_data:
        self.CopyTestData([data], self.adb.GetExternalStorage())
    if self.test_package.test_suite_basename == 'webkit_unit_tests':
      self.PushWebKitUnitTestsData()

  def PushWebKitUnitTestsData(self):
    """Pushes the webkit_unit_tests data files to the device.

    The path of this directory is different when the suite is being run as
    part of a WebKit check-out.
    """
    webkit_src = os.path.join(constants.CHROME_DIR, 'third_party', 'WebKit')
    if self.in_webkit_checkout:
      webkit_src = os.path.join(constants.CHROME_DIR, '..', '..', '..')

    self.adb.PushIfNeeded(
        os.path.join(webkit_src, 'Source/WebKit/chromium/tests/data'),
        os.path.join(
            self.adb.GetExternalStorage(),
            'third_party/WebKit/Source/WebKit/chromium/tests/data'))

  def RunTests(self):
    """Runs tests on a single device.

    Returns:
      A TestResults object.
    """
    try:
      self.test_package.CreateTestRunnerScript(self._gtest_filter,
                                               self._test_arguments)
      self.test_results = self.test_package.RunTestsAndListResults()
    except errors.DeviceUnresponsiveError as e:
      # Make sure this device is not attached
      if android_commands.IsDeviceAttached(self.device):
        raise e

      # TODO(frankf): We should report these as "skipped" not "failures".
      # Wrap the results
      logging.warning(e)
      failed_tests = []
      for t in self._gtest_filter.split(':'):
        failed_tests += [BaseTestResult(t, '')]
      self.test_results = TestResults.FromRun(
          failed=failed_tests, device_exception=self.device)

    return self.test_results

  def SetUp(self):
    """Sets up necessary test enviroment for the test suite."""
    super(SingleTestRunner, self).SetUp()
    self.adb.ClearApplicationState(constants.CHROME_PACKAGE)
    if self.dump_debug_info:
      self.dump_debug_info.StartRecordingLog(True)
    self.StripAndCopyFiles()
    self.LaunchHelperToolsForTestSuite()
    self.tool.SetupEnvironment()

  def TearDown(self):
    """Cleans up the test enviroment for the test suite."""
    self.tool.CleanUpEnvironment()
    if self.test_package.cleanup_test_files:
      self.adb.RemovePushedFiles()
    if self.dump_debug_info:
      self.dump_debug_info.StopRecordingLog()
    if self.dump_debug_info:
      self.dump_debug_info.ArchiveNewCrashFiles()
    super(SingleTestRunner, self).TearDown()
