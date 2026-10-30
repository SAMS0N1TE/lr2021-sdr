import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import serial
from continuous_capture import main


class CaptureFailureTests(unittest.TestCase):
    def test_invalid_experimental_combinations_do_not_open_serial(self):
        for args in (['--spi', '16', '--rx-delay', '1'],
                     ['--format', 'iq8', '--bandwidth', '31.25'],
                     ['--format', 'iq16', '--bandwidth', '31.25', '--decode']):
            with patch('continuous_capture.serial.Serial') as port, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    main(args)
                port.assert_not_called()

    def test_disconnect_during_setup_preserves_failure_report(self):
        class DisconnectedPort:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                pass

            def reset_input_buffer(self):
                raise serial.SerialException('disconnected')

            def write(self, data):
                raise serial.SerialException('still disconnected')

        with tempfile.TemporaryDirectory() as directory:
            with patch('continuous_capture.serial.Serial', return_value=DisconnectedPort()), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as exited:
                    main(['--seconds', '1', '--out', directory])
            self.assertEqual(exited.exception.code, 1)
            paths = list(Path(directory).glob('*/summary.json'))
            self.assertEqual(len(paths), 1)
            report = json.loads(paths[0].read_text())
            self.assertFalse(report['valid'])
            self.assertIn('disconnected', report['error'])
            self.assertIn('still disconnected', report['stop_error'])


if __name__ == '__main__':
    unittest.main()
