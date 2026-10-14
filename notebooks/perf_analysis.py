"""读取已有诊断日志和测量文件；分位数与请求分析复用仓库分析器。"""
import csv
import importlib.util
import json
import math
from collections import defaultdict
from pathlib import Path


_analysis_path = Path(__file__).resolve().parents[1] / 'tests/02_reproducible_measurement/analyze.py'
_spec = importlib.util.spec_from_file_location('trade_request_analysis', _analysis_path)
_analysis = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_analysis)


def summarize(source, kind, values, unit):
    stats = _analysis.distribution(values)
    return {'source': source, 'kind': kind, 'unit': unit, 'n': stats['n'],
            **{name: stats[name + '_ns'] for name in ('mean', 'p50', 'p95', 'p99', 'max')
               if name + '_ns' in stats}}


def read_traces(root, tsc_hz=None):
    if tsc_hz is not None and (not isinstance(tsc_hz, (int, float)) or isinstance(tsc_hz, bool)
                               or not math.isfinite(tsc_hz) or tsc_hz <= 0):
        raise ValueError('TSC_HZ 必须是实测的正数 Hz；不是 CPU 标称主频')
    groups, ttt, issues = defaultdict(list), defaultdict(list), []
    for path in sorted(root.rglob('*.log')):
        source = path.relative_to(root).as_posix()
        try:
            with path.open(encoding='utf-8') as handle:
                for line_number, line in enumerate(handle, 1):
                    tokens = line.split()
                    if len(tokens) < 2 or tokens[1] not in ('RDTSC', 'TTT'):
                        continue
                    try:
                        if len(tokens) != 4:
                            raise ValueError('计时行应为：时间 类型 标签 整数')
                        value = int(tokens[3])
                        if value < 0:
                            raise ValueError('计时值不能为负')
                        key = (source, tokens[2])
                        if tokens[1] == 'RDTSC':
                            groups[key].append(value if tsc_hz is None else value * 1e9 / tsc_hz)
                        else:
                            ttt[key].append(value)
                    except ValueError as error:
                        issues.append(f'{source}:{line_number}: {error}')
        except (OSError, UnicodeError) as error:
            issues.append(f'{source}: {error}')
    unit = 'TSC ticks' if tsc_hz is None else 'ns (calibrated TSC)'
    rows = [summarize(source, tag, values, unit) for (source, tag), values in groups.items()]
    timeline = [{'source': source, 'tag': tag, 'n': len(values),
                 'min_epoch_ns': min(values), 'max_epoch_ns': max(values)}
                for (source, tag), values in ttt.items()]
    series = {f'{source} :: {tag}': {'values': values, 'unit': unit}
              for (source, tag), values in groups.items()}
    return {'rdtsc': rows, 'ttt': timeline, 'series': series, 'issues': issues}


def _component_csv(path, fieldnames):
    values = []
    with path.open(newline='', encoding='utf-8') as handle:
        for index, row in enumerate(csv.DictReader(handle)):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f'第 {index + 2} 行 CSV 列数与表头不一致')
            elapsed = int(row['elapsed_ns'])
            if elapsed < 0:
                raise ValueError(f'第 {index + 2} 行耗时为负')
            if fieldnames == ['operation', 'elapsed_ns']:
                if int(row['operation']) != index:
                    raise ValueError(f'第 {index + 2} 行 operation 不连续或重复')
                values.append(elapsed)
            else:
                if int(row['cycle']) != index or int(row['pairs']) <= 0:
                    raise ValueError(f'第 {index + 2} 行周期或 pairs 无效')
                values.append(elapsed / int(row['pairs']))
    if not values:
        raise ValueError('组件 CSV 没有样本')
    return values


def _producer_result(path):
    result_path = path.parent / 'result.txt'
    if not result_path.exists():
        return None
    found = None
    for line in result_path.read_text(encoding='utf-8').splitlines():
        if line.lstrip().startswith('{'):
            value = json.loads(line)
            if isinstance(value, dict) and value.get('name') == path.stem:
                if found is not None:
                    raise ValueError('result.txt 中有重复指标名')
                found = value
    return found


def _request_csv(path, window_ns):
    metadata_path = path.parent / 'result.json'
    metadata = json.loads(metadata_path.read_text(encoding='utf-8')) if metadata_path.exists() else {}
    if not isinstance(metadata, dict):
        raise ValueError('result.json 应为对象')
    saved_key = 'failed_statistics' if path.name == 'failed_events.csv' else 'statistics'
    saved = metadata.get(saved_key)
    if saved is not None:
        if not isinstance(saved, dict):
            raise ValueError('元数据中的 statistics 应为对象')
        start, end = saved['window_start_ns'], saved['window_end_ns']
    elif window_ns is not None:
        start, end = window_ns
    else:
        raise ValueError('缺少 result.json 的统计窗口；可显式填写 REQUEST_WINDOW_NS')
    if type(start) is not int or type(end) is not int:
        raise ValueError('统计窗口必须使用整数单调时钟纳秒')
    with path.open(newline='', encoding='utf-8') as handle:
        for index, event in enumerate(csv.DictReader(handle)):
            if None in event or any(value is None for value in event.values()):
                raise ValueError(f'第 {index + 2} 行 CSV 列数与表头不一致')
    stats = _analysis.analyze(path, start, end)
    if saved is not None and stats != saved:
        raise ValueError('CSV 重算结果与 result.json 不一致，不能沿用原验收状态')
    state = 'passed' if metadata.get('correctness_ok') is True else (
        'failed' if metadata.get('correctness_ok') is False else 'not provided')
    rtt = stats['actual_send_to_response']
    row = {'correctness': state, 'offered': stats['window_counts']['offered'],
           'completed_cohort': sum(stats['cohort'][key] for key in ('ACCEPTED', 'CANCELED', 'REJECTED')),
           'timeouts': stats['cohort']['TIMEOUT'], 'unsent': stats['cohort']['UNSENT'],
           'window_completed': stats['window_counts']['completed'],
           'window_completed_per_s': stats['window_rates_per_s']['completed'],
           'completed_rtt_n': rtt['n'],
           'completed_rtt_p50_ms': rtt.get('p50_ns', 0) / 1e6 if rtt['n'] else None,
           'completed_rtt_p99_ms': rtt.get('p99_ns', 0) / 1e6 if rtt['n'] else None,
           'completed_rtt_max_ms': rtt.get('max_ns', 0) / 1e6 if rtt['n'] else None,
           'all_offered_scheduled_p99_ns': stats['all_offered_p99_ns'],
           'all_offered_p99_status': stats['all_offered_p99_status']}
    values = []
    with path.open(newline='', encoding='utf-8') as handle:
        for event in csv.DictReader(handle):
            if start <= int(event['scheduled_ns']) < end and event['completed_ns']:
                values.append(int(event['completed_ns']) - int(event['sent_ns']))
    return row, stats, values


def analyze_directory(directory=None, tsc_hz=None, request_window_ns=None):
    empty = {'rdtsc': [], 'ttt': [], 'components': [], 'requests': [],
             'request_statistics': {}, 'series': {}, 'issues': []}
    if directory is None:
        empty['issues'].append('未设置 TRADE_ANALYSIS_DIR；选择某一次运行或组件输出目录。')
        return empty
    root = Path(directory).expanduser().resolve()
    if not root.is_dir():
        raise ValueError(f'数据目录不存在或不是目录：{root}')
    traces = read_traces(root, tsc_hz)
    result = {**empty, **{key: traces[key] for key in ('rdtsc', 'ttt', 'series', 'issues')}}
    request_fields = {'request_id', 'kind', 'scheduled_ns', 'sent_ns', 'completed_ns', 'status'}
    for path in sorted(root.rglob('*.csv')):
        source = path.relative_to(root).as_posix()
        try:
            if path.name == 'warmup_events.csv':
                result['issues'].append(f'{source}: 预热请求单独保留，不纳入正式统计窗口。')
                continue
            with path.open(newline='', encoding='utf-8') as handle:
                fields = csv.DictReader(handle).fieldnames or []
            if request_fields.issubset(fields):
                row, stats, values = _request_csv(path, request_window_ns)
                result['requests'].append({'source': source, **row})
                result['request_statistics'][source] = stats
                result['series'][source] = {'values': values, 'unit': 'ns (completed RTT)'}
            elif fields in (['operation', 'elapsed_ns'], ['cycle', 'pairs', 'elapsed_ns']):
                values = _component_csv(path, fields)
                is_cycle = fields[0] == 'cycle'
                unit = 'ns/pair (cycle mean)' if is_cycle else 'ns/sample'
                row = summarize(source, 'pool cycle mean' if is_cycle else path.stem, values, unit)
                producer = _producer_result(path)
                row['producer_check'] = 'matched' if producer is not None else 'not provided'
                if producer is not None:
                    if producer.get('errors') != 0:
                        raise ValueError('组件生产程序报告 errors 非零')
                    if producer.get('operations') != row['n'] or any(
                            producer.get(key + '_ns') != row[key] for key in ('p50', 'p95', 'p99', 'max')):
                        raise ValueError('原始 CSV 的计数／分位数与 result.txt 不一致')
                result['components'].append(row)
                result['series'][source] = {'values': values, 'unit': unit}
            else:
                result['issues'].append(f'{source}: 不支持的 CSV 列，未当作延迟样本：{fields}')
        except (ValueError, KeyError, TypeError, OSError, UnicodeError, csv.Error) as error:
            result['issues'].append(f'{source}: {error}')
    if not any(result[key] for key in ('rdtsc', 'ttt', 'components', 'requests')):
        result['issues'].append('没有可分析数据。默认 Release 关闭诊断计时；普通日志为空不代表零延迟。')
    return result


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--tsc-hz', type=float, default=None)
    args = parser.parse_args()
    output = analyze_directory(args.directory, args.tsc_hz)
    output.pop('series')
    print(json.dumps(output, ensure_ascii=False, indent=2))
