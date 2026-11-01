#include "ble/att.h"

#include "core/bytes.h"

namespace pb::ble {

AttServer::AttServer(GattDb& db) : db_(db) {}

std::vector<uint8_t> AttServer::error(uint8_t req, uint16_t handle, uint8_t code) {
  ByteWriter w;
  w.u8(ATT_ERROR);
  w.u8(req);
  w.u16le(handle);
  w.u8(code);
  return w.take();
}

std::vector<uint8_t> AttServer::build_notification(uint16_t handle,
                                                   const std::vector<uint8_t>& value) {
  ByteWriter w;
  w.u8(ATT_HANDLE_VALUE_NTF);
  w.u16le(handle);
  w.bytes(value.data(), value.size());
  return w.take();
}

std::optional<std::vector<uint8_t>> AttServer::handle(const uint8_t* pdu, size_t n) {
  if (n < 1) return error(0, 0, E_INVALID_PDU);
  uint8_t op = pdu[0];
  ByteReader r(pdu + 1, n - 1);

  switch (op) {
    case ATT_MTU_REQ: {
      uint16_t client_mtu = r.u16le();
      // 应答 = 服务器自身 ATT_MTU；有效 MTU = min(247, client)
      db_.mtu = std::max<uint16_t>(23, std::min<uint16_t>(247, client_mtu));
      ByteWriter w;
      w.u8(ATT_MTU_RSP);
      w.u16le(247);
      return w.take();
    }
    case ATT_READ_BY_GROUP_REQ: {
      uint16_t start = r.u16le(), end = r.u16le(), type = r.u16le();
      if (type != T_PRIMARY_SVC) return error(op, start, E_UNSUPPORTED_REQ);
      // 条目 {start, end, uuid16} = 6B；按 mtu-2 容量截断
      ByteWriter w;
      w.u8(ATT_READ_BY_GROUP_RSP);
      w.u8(6);
      int fit = std::max(1, (db_.mtu - 2) / 7);  // 1 op + 1 len + n×6
      bool any = false;
      for (auto& [s, e] : db_.service_ranges()) {
        if (s > end || e < start) continue;
        if (fit-- <= 0) break;
        const GattAttr* a = db_.by_handle(s);
        w.u16le(s);
        w.u16le(e);
        w.bytes(a->value.data(), std::min<size_t>(a->value.size(), 2));
        any = true;
      }
      if (!any) return error(op, start, E_ATTR_NOT_FOUND);
      return w.take();
    }
    case ATT_READ_BY_TYPE_REQ: {
      uint16_t start = r.u16le(), end = r.u16le(), type = r.u16le();
      // char decl（0x2803）或值类型（0x2Axx）
      if (type == T_CHAR_DECL) {
        ByteWriter w;
        w.u8(ATT_READ_BY_TYPE_RSP);
        w.u8(7);  // handle(2)+props(1)+value_handle(2)+value_type(2)
        int fit = std::max(1, (db_.mtu - 2) / 8);
        bool any = false;
        // 逐句柄扫描 char decl（db_ 只读接口）。
        // 用 32 位迭代：end=0xFFFF 时 uint16_t 会回绕重扫（重复句柄/死循环）
        for (uint32_t h = start; h <= (uint32_t)end && fit > 0; ++h) {
          const GattAttr* a = db_.by_handle((uint16_t)h);
          if (!a || a->type != T_CHAR_DECL) continue;
          w.u16le((uint16_t)h);
          w.u8(a->props);
          // 值句柄 = 下一个属性
          const GattAttr* v = db_.by_handle((uint16_t)(h + 1));
          w.u16le(v ? v->handle : 0);
          w.u16le(a->value_type);
          --fit;
          any = true;
        }
        if (!any) return error(op, start, E_ATTR_NOT_FOUND);
        return w.take();
      }
      // 值类型查询
      ByteWriter w;
      bool any = false;
      const GattAttr* first = db_.find_value_by_type(type, (uint16_t)(start - 1));
      if (!first || first->handle > end) return error(op, start, E_ATTR_NOT_FOUND);
      auto [data, err] = db_.read_value(first->handle, 0);
      if (err) return error(op, first->handle, err);
      size_t dlen = std::min<size_t>(data.size(), db_.mtu - 3);
      w.u8(ATT_READ_BY_TYPE_RSP);
      w.u8((uint8_t)(2 + dlen));
      w.u16le(first->handle);
      w.bytes(data.data(), dlen);
      any = true;
      (void)any;
      return w.take();
    }
    case ATT_READ_REQ: {
      uint16_t h = r.u16le();
      auto [data, err] = db_.read_value(h, 0);
      if (err) return error(op, h, err);
      ByteWriter w;
      w.u8(ATT_READ_RSP);
      size_t dlen = std::min<size_t>(data.size(), db_.mtu - 1);
      w.bytes(data.data(), dlen);
      return w.take();
    }
    case ATT_READ_BLOB_REQ: {
      uint16_t h = r.u16le();
      uint16_t off = r.u16le();
      auto [data, err] = db_.read_value(h, off);
      if (err) return error(op, h, err);
      ByteWriter w;
      w.u8(ATT_READ_BLOB_RSP);
      size_t dlen = std::min<size_t>(data.size(), db_.mtu - 1);
      w.bytes(data.data(), dlen);
      return w.take();
    }
    case ATT_FIND_INFO_REQ: {
      uint16_t start = r.u16le(), end = r.u16le();
      auto descs = db_.descriptors_in_range(start, end);
      if (descs.empty()) return error(op, start, E_ATTR_NOT_FOUND);
      ByteWriter w;
      w.u8(ATT_FIND_INFO_RSP);
      w.u8(0x01);  // 16-bit UUID 短格式
      int fit = std::max(1, (db_.mtu - 2) / 4);
      for (auto& [h, u] : descs) {
        if (fit-- <= 0) break;
        w.u16le(h);
        w.u16le(u);
      }
      return w.take();
    }
    case ATT_WRITE_REQ: {
      uint16_t h = r.u16le();
      // 载荷起点 min(n,3)：n<3（opcode+残缺句柄）时 pdu+3 越过 pdu+n，
      // vector(first,last) 区间起点越过终点是 UB —— 退化为空载荷（句柄 0 → 由 DB 报错）
      size_t data_off = std::min<size_t>(n, 3);
      std::vector<uint8_t> v(pdu + data_off, pdu + n);
      uint8_t err = db_.write_value(h, v);
      if (err) return error(op, h, err);
      return std::vector<uint8_t>{ATT_WRITE_RSP};
    }
    case ATT_WRITE_CMD: {
      uint16_t h = r.u16le();
      size_t data_off = std::min<size_t>(n, 3);  // 同上：防区间 UB
      std::vector<uint8_t> v(pdu + data_off, pdu + n);
      db_.write_value(h, v);  // 无应答（错误静默）
      return std::nullopt;
    }
    case ATT_READ_MULTI_REQ:
    case ATT_FIND_BY_TYPE_REQ:
    case ATT_PREPARE_WRITE_REQ:
    case ATT_EXECUTE_WRITE_REQ:
      return error(op, 0, E_UNSUPPORTED_REQ);
    default:
      return error(op, 0, E_INVALID_PDU);
  }
}

}  // namespace pb::ble
