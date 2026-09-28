#!/usr/bin/env bash
# 停止 CloudDisk 的应用进程 (不停止 MySQL/RabbitMQ 系统服务)。
set -u
echo "停止 server / UserService / backup ..."
pkill -f '(^|/)server$'      2>/dev/null && echo "  server 已停止"
pkill -f '(^|/)UserService$' 2>/dev/null && echo "  UserService 已停止"
pkill -f '(^|/)backup$'      2>/dev/null && echo "  backup 已停止"
echo "停止 Consul dev agent ..."
pkill -f 'consul agent -dev' 2>/dev/null && echo "  consul 已停止"
echo "完成。(MySQL / RabbitMQ 系统服务未动, 如需停止: sudo systemctl stop mysql rabbitmq-server)"
