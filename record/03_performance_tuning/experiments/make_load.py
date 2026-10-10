from pathlib import Path
root = Path('/home/zjh/XiangMu/TradeSystem-rebuild')
source = (root / 'record/02_reproducible_measurement/load_generator.py').read_text()
source = source.replace("env = dict(os.environ, TRADE_SNAPSHOT_MS='60000')", "env = dict(os.environ, TRADE_SNAPSHOT_MS=str(config.get('snapshot_ms', 60000)))")
source = source.replace("warm_count = 32 * config['clients']", "warm_count = config.get('warm_requests', 32 * config['clients'])")
source = source.replace("drive(peers, warmup, 'closed', 0, 5)", "drive(peers, warmup, config.get('warm_mode', 'closed'), config.get('warm_rate', 0), 30)")
source = source.replace("config['seed'], 16)", "config['seed'], warm_count // (2 * config['clients']))")
source = source.replace("Path(__file__).parents[1] / '01_reliability/reconcile.py'", "Path('/home/zjh/XiangMu/TradeSystem-rebuild/record/01_reliability/reconcile.py')")
out = Path(__file__).parent / 'tuning_load.py'
out.write_text(source, encoding='utf-8', newline='\n')
print(out)
