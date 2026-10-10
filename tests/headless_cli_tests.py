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
    # Execution profiles: --profile selects, wins over OP1EMU_PROFILE, and is validated.
    import os
    script.write_text('quit\n')
    def run_env(args, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith('OP1EMU_')}
        env.update(extra)
        return subprocess.run(args, cwd=root, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30, env=env)
    result = run_env(command + ['--profile', 'strict'])
    assert result.returncode == 0 and 'Emulator profile=strict assumptions=0' in result.stdout+result.stderr, result.stdout+result.stderr
    result = run_env(command + ['--profile', 'functional'])
    output = result.stdout+result.stderr
    assert result.returncode == 0 and 'Emulator profile=functional assumptions=6' in output and 'ASSUMED: external SPORT0 audio clock corresponds to 44.1 kHz native frame rate' in output and 'SPORT guest-clock pacing: 44100 frames/s' in output and 'ASSUMED: CoreTimer continues during plain IDLE' in output and 'CLKIN=25000000 Hz: PROVISIONAL' in output, output
    assert run_env(command + ['--profile', 'lenient']).returncode == 1
    assert run_env(command, OP1EMU_PROFILE='lenient').returncode == 1
    assert run_env(command, OP1EMU_PROFILE='strict', OP1EMU_EXPERIMENTAL_CLKIN_HZ='25000000').returncode == 1
    result = run_env(command + ['--profile', 'functional'], OP1EMU_PROFILE='strict', OP1EMU_EXPERIMENTAL_CLKIN_HZ='25000000')
    assert result.returncode == 0 and 'Emulator profile=functional' in result.stdout+result.stderr, 'CLI profile wins over OP1EMU_PROFILE'
    result = run_env(command)
    assert result.returncode == 0 and 'Emulator profile=unset assumptions=0' in result.stdout+result.stderr
    assert source.read_bytes() == bytes([255])*128
print('headless CLI: clean exit, malformed input, missing snapshot, bounded frame timeout, and profiles passed')
