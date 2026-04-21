本项目是一个从零构建的、面向低延迟场景的完整交易生态系统，包含模拟交易所与算法交易客户端。  
所有模块通过自研无锁环形队列通信，关键路径采用内存池、CPU 亲和性绑定优化，支持 UDP 多播行情、TCP 订单通道、快照同步恢复、FIFO 公平定序及实时风控。  
**交易所端：** 撮合引擎、行情发布器（增量 + 快照）、订单服务器  
**客户端：** 行情消费者、本地订单簿维护、策略引擎、订单网关、风控与头寸追踪  
整个系统在 Linux 环境 下运行，模块间通过无锁队列解耦，可一键启动多客户端联合压测，并输出端到端延迟分析图表。  
项目原型来自Sourav Ghosh的《Building Low Latency Applications with C++》，在啃完此书后，对其中一部分代码进行了优化，如无锁队列的代码，优化了逻辑，并新增了acquire/release内存序。  
## 项目架构图
<img width="1000" height="800" alt="image" src="https://github.com/user-attachments/assets/50439269-08c6-458d-9cef-94da2b6dc0ba" />

### 交易所端架构
<img width="1000" height="400" alt="image" src="https://github.com/user-attachments/assets/e5a5adc8-6e60-498f-ab9e-4f709a0e02a7" />  

其中，市场数据发布器的内部架构和订单服务器的内部架构如下图  

<img width="1000" height="500" alt="image" src="https://github.com/user-attachments/assets/2678a2c7-ce22-47f7-bb51-ab4e721bc292" />

<img width="1000" height="500" alt="image" src="https://github.com/user-attachments/assets/bb3d7ab3-a239-426e-8848-f6a3e7004ecf" />

### 客户端架构  
#### 市场数据接收器
<img width="1000" height="580" alt="image" src="https://github.com/user-attachments/assets/dc5b59bc-1977-40d0-bce6-821b0683eb24" />

#### 订单网关
<img width="1000" height="580" alt="image" src="https://github.com/user-attachments/assets/9b3c39eb-5742-4cb2-9602-90a071631387" />

### 核心的交易引擎
<img width="900" height="460" alt="image" src="https://github.com/user-attachments/assets/bbac8da4-d3c0-40aa-8bce-38ec28069bc0" />
