import importlib.machinery
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / 'sensor-present'
loader = importlib.machinery.SourceFileLoader('sensor_present', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
sensor_present = importlib.util.module_from_spec(spec)
loader.exec_module(sensor_present)


class SensorPresenceTests(unittest.TestCase):
    def probe(self, version, attached):
        def ioctl(fd, request, buffer, mutate):
            self.assertEqual(fd, 9)
            self.assertEqual(request, sensor_present.GET_INFO)
            self.assertTrue(mutate)
            struct.pack_into('=II', buffer, 0, version, attached)

        with patch.object(sensor_present.os, 'open', return_value=9), \
             patch.object(sensor_present.os, 'close') as close, \
             patch.object(sensor_present.fcntl, 'ioctl', side_effect=ioctl):
            result = sensor_present.present('/dev/test-reader')
        close.assert_called_once_with(9)
        return result

    def test_supported_interfaces_require_a_physical_reader(self):
        for version in (4, 5):
            with self.subTest(version=version):
                self.assertTrue(self.probe(version, 1))
                for sensor in (0, 2):
                    self.assertFalse(self.probe(version, sensor))
        for version in (0, 3, 6, 0xffffffff):
            self.assertFalse(self.probe(version, 1))

    def test_ioctl_failure_closes_the_device(self):
        with patch.object(sensor_present.os, 'open', return_value=9), \
             patch.object(sensor_present.os, 'close') as close, \
             patch.object(sensor_present.fcntl, 'ioctl', side_effect=OSError):
            with self.assertRaises(OSError):
                sensor_present.present('/dev/test-reader')
        close.assert_called_once_with(9)

    def test_missing_device_is_not_advertised(self):
        with patch.object(sensor_present.os, 'open', side_effect=FileNotFoundError):
            with self.assertRaises(FileNotFoundError):
                sensor_present.present('/dev/missing')


if __name__ == '__main__':
    unittest.main()
