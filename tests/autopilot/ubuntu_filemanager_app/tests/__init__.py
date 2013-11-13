# -*- Mode: Python; coding: utf-8; indent-tabs-mode: nil; tab-width: 4 -*-
# Copyright 2013 Canonical
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 3, as published
# by the Free Software Foundation.

"""Filemanager app autopilot tests."""

import mock
import os.path
import shutil
import tempfile

from autopilot.input import Mouse, Touch, Pointer
from autopilot.platform import model
from autopilot.testcase import AutopilotTestCase

from ubuntu_filemanager_app import emulators

from ubuntuuitoolkit import (
    base,
    emulators as toolkit_emulators
)


class FileManagerTestCase(AutopilotTestCase):

    """A common test case class that provides several useful methods for
    filemanager-app tests.

    """
    if model() == 'Desktop':
        scenarios = [('with mouse', dict(input_device_class=Mouse))]
    else:
        scenarios = [('with touch', dict(input_device_class=Touch))]

    local_location = "../../ubuntu-filemanager-app.qml"
    installed_location = "/usr/share/ubuntu-filemanager-app/" \
                         "ubuntu-filemanager-app.qml"

    def setup_environment(self):
        if os.path.exists(self.local_location):
            launch = self.launch_test_local
            test_type = 'local'
        elif os.path.exists(self.installed_location):
            launch = self.launch_test_installed
            test_type = 'deb'
        else:
            launch = self.launch_test_click
            test_type = 'click'
        return launch, test_type

    def setUp(self):
        launch, self.test_type = self.setup_environment()
        if self.test_type != 'click':
            self._patch_home()
        self.pointing_device = Pointer(self.input_device_class.create())
        super(FileManagerTestCase, self).setUp()
        self.original_file_count = \
            len([i for i in os.listdir(os.environ['HOME'])
                 if not i.startswith('.')])
        launch()

    def _patch_home(self):
        #create a temporary home for testing purposes
        temp_dir = tempfile.mkdtemp()
        #if the Xauthority file is in home directory
        #make sure we copy it to temp home, otherwise do nothing
        xauth = os.path.expanduser(os.path.join('~', '.Xauthority'))
        if os.path.isfile(xauth):
            shutil.copyfile(
                os.path.expanduser(os.path.join('~', '.Xauthority')),
                os.path.join(temp_dir, '.Xauthority'))
        self.addCleanup(shutil.rmtree, temp_dir)
        patcher = mock.patch.dict('os.environ', {'HOME': temp_dir})
        patcher.start()
        self.addCleanup(patcher.stop)

    def launch_test_local(self):
        self.app = self.launch_test_application(
            base.get_qmlscene_launch_command(),
            self.local_location,
            app_type='qt',
            emulator_base=toolkit_emulators.UbuntuUIToolkitEmulatorBase)

    def launch_test_installed(self):
        self.app = self.launch_test_application(
            base.get_qmlscene_launch_command(),
            self.installed_location,
            "--desktop_file_hint="
            "/usr/share/applications/ubuntu-filemanager-app.desktop",
            app_type='qt',
            emulator_base=toolkit_emulators.UbuntuUIToolkitEmulatorBase)

    def launch_test_click(self):
        self.app = self.launch_click_package(
            "com.ubuntu.filemanager",
            emulator_base=toolkit_emulators.UbuntuUIToolkitEmulatorBase)

    @property
    def main_view(self):
        return self.app.select_single(emulators.MainView)