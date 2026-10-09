import pathlib, subprocess, sys, tempfile

binary, config = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='op1-headless-cli-') as directory:
    root = pathlib.Path(directory)
    source = root/'source.nand'
    source.write_bytes(bytes([255])*128)
    script = root/'input.txt'
    script.write_text('accel 64 -64 512\nvolume 128\nquit\n')
    command = [binary, str(source), '--ui-config', config, '--input-script', str(script)]
    def run(args): return subprocess.run(args, cwd=root, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
    result = run(command)
    assert result.returncode == 0, result.stdout+result.stderr
    assert source.read_bytes() == bytes([255])*128
    assert run([binary, '--help']).returncode == 0
    assert run([binary, '--unknown']).returncode == 1
    script.write_text('press missing-key\n')
    assert run(command).returncode == 1
    missing = root/'missing.nand'
    script.write_text('quit\n')
    assert run([binary, str(missing), '--ui-config', config, '--input-script', str(script)]).returncode == 1
    assert not missing.exists()
    script.write_text('wait-frames 1 1\nquit\n')
    result = run(command)
    assert result.returncode == 1 and 'Timed out waiting for frames' in result.stdout+result.stderr
print('headless CLI: clean exit, malformed input, missing snapshot, and bounded frame timeout passed')
