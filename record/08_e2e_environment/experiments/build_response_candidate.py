import pathlib,subprocess,tarfile,io
r=pathlib.Path('/home/zjh/XiangMu/TradeSystem-rebuild');e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition');s=e/'response-candidate-source';s.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive','HEAD'],cwd=r))) as t:t.extractall(s,filter='data')
p=s/'exchange/order_server/order_server.h';old=p.read_text();start=old.index('        bool queued_response = false;');end=old.index('\n      } //',start);block=old[start:end]
text=old[:start]+'        drainResponses(); // 先前接收阶段结束后，继续推进回报。'+old[end:]
marker='    bool reject('
method='    void drainResponses() noexcept { // 只由订单服务器线程消费回报；不会调用接收回调。\n'+ '\n'.join(line[4:] for line in block.splitlines())+'\n    } // 结束回报推进。\n\n'
text=text.replace(marker,method+marker,1)
anchor='      while(consumed+Common::Wire::RequestSize<=socket->next_rcv_valid_index_) { // 只解析完整 RequestSize 帧，半帧留待补齐。'
assert anchor in text
text=text.replace(anchor,anchor+'\n        drainResponses(); // 下一帧可能等待审计落盘，先发送已经完成的回报。',1)
p.write_text(text)
diff=subprocess.run(['diff','-u','--label','a/exchange/order_server/order_server.h','--label','b/exchange/order_server/order_server.h',str(r/'exchange/order_server/order_server.h'),str(p)],capture_output=True,text=True)
assert diff.returncode==1;(e/'response-candidate.patch').write_text(diff.stdout)
b=e/'response-candidate-build'
with (e/'response-candidate-build.log').open('w') as out:
 subprocess.run(['cmake','-S',str(s),'-B',str(b),'-DCMAKE_BUILD_TYPE=Release','-DTRADE_PERF_TRACE_EVERY=1'],stdout=out,stderr=subprocess.STDOUT,check=True)
 subprocess.run(['cmake','--build',str(b),'-j2'],stdout=out,stderr=subprocess.STDOUT,check=True)
with (e/'response-candidate-ctest.log').open('w') as out:
 result=subprocess.run(['ctest','--test-dir',str(b),'--output-on-failure'],stdout=out,stderr=subprocess.STDOUT)
print('candidate ctest exit',result.returncode,flush=True)
raise SystemExit(result.returncode)
