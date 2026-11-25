/*
 * mt76tm.c —— 通过 nl80211 TESTMODE 控制 mt76 连续发射（纯物理层信号）
 *
 * 用途：
 *   把 mt7915/7976 切到 MT76_TM_STATE_TX_CONT（连续发射），在指定信道上
 *   发出没有时间间隙的纯物理层信号 —— 也就是连续波。这是干扰器的理想模式：
 *   没有 CCA、没有帧间隔、没有 beacon 定时器。
 *
 * 不依赖 libnl，直接操作 generic netlink，交叉编译后约 20KB，
 * 直接丢到路由器 /usr/bin/ 就能跑。
 *
 * 前提：内核必须用 CONFIG_NL80211_TESTMODE=y 编译，
 *       否则 mt7915e 里没有 testmode.o，本工具返回 EOPNOTSUPP(-95)。
 *       证据：mt7915/Makefile 里 `mt7915e-$(CONFIG_NL80211_TESTMODE) += testmode.o`
 *
 * 用法：
 *   mt76tm status <ifname>             探测 testmode 是否可用
 *   mt76tm check  <ifname>             只读：读回内核里的当前 testmode 状态
 *   mt76tm cont   <ifname> [选项]      连续波（纯载波）
 *   mt76tm frames <ifname> [选项]      灌包（走 MAC 队列，duty/ipg 可调）
 *   mt76tm off    <ifname>             停止，恢复正常工作
 *
 * 两个坑（都实测踩过，改代码前先看）：
 *  1. MT76_TM_ATTR_STATE 的 nla_policy 是 NLA_U8，而驱动用 nla_get_u32 读。
 *     必须按 u8 发！按 u32 发会被 nla_parse_deprecated 判为长度非法并**丢弃**，
 *     命令照样返回 0，但状态没变 —— 看起来"成功"，实际一点波都不发。
 *  2. 所以设完状态必须用 dump 读回 state 核对（本工具的 check/cont/frames 都自动做）。
 *
 * 连续发射选项：
 *   -c <信道>   1-13，默认 1
 *   -w <带宽>   20|40|80，默认 20
 *   -m <模式>   cck|ofdm|ht|vht|he
 *   -M <MCS>    0-11
 *   -n <流数>   1-2
 *   -a <天线>   1|3
 *   -p <功率>   0-31，默认 20
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>

/* ------------------------------------------------------------------
 * NLA_OK / NLA_NEXT 补丁
 *
 * 这两个宏属于 libnl，**不在内核 UAPI 头文件里**。
 * 内核的 <linux/netlink.h> 只提供 NLA_ALIGN / NLA_HDRLEN，
 * 直接用 NLA_OK/NLA_NEXT 会编译报 "undeclared" —— 这是第一次云端
 * 编译失败的原因（用户包在工具链和目标固件之后才编，所以跑到最后才炸）。
 * 这里按 libnl 的定义自己补上，不引入 libnl 依赖。
 * ------------------------------------------------------------------ */
#ifndef NLA_OK
#define NLA_OK(nla, len) \
	((len) >= (int)sizeof(struct nlattr) && \
	 (nla)->nla_len >= sizeof(struct nlattr) && \
	 (nla)->nla_len <= (len))
#endif

#ifndef NLA_NEXT
#define NLA_NEXT(nla, attrlen) \
	((attrlen) -= NLA_ALIGN((nla)->nla_len), \
	 (struct nlattr *)(((char *)(nla)) + NLA_ALIGN((nla)->nla_len)))
#endif

/* ------------------------------------------------------------------
 * mt76 testmode 属性号（不来自 uapi，抄自
 * drivers/net/wireless/mediatek/mt76/testmode.h，该枚举多年未变）
 * ------------------------------------------------------------------ */
enum {
	MT76_TM_ATTR_UNSPEC,
	MT76_TM_ATTR_RESET,
	MT76_TM_ATTR_STATE,
	MT76_TM_ATTR_MTD_PART,
	MT76_TM_ATTR_MTD_OFFSET,
	MT76_TM_ATTR_TX_COUNT,
	MT76_TM_ATTR_TX_LENGTH,
	MT76_TM_ATTR_TX_RATE_MODE,
	MT76_TM_ATTR_TX_RATE_NSS,
	MT76_TM_ATTR_TX_RATE_IDX,
	MT76_TM_ATTR_TX_RATE_SGI,
	MT76_TM_ATTR_TX_RATE_LDPC,
	MT76_TM_ATTR_TX_RATE_STBC,
	MT76_TM_ATTR_TX_LTF,
	MT76_TM_ATTR_TX_ANTENNA,
	MT76_TM_ATTR_TX_POWER_CONTROL,
	MT76_TM_ATTR_TX_POWER,
	MT76_TM_ATTR_FREQ_OFFSET,
	MT76_TM_ATTR_STATS,
	MT76_TM_ATTR_TX_SPE_IDX,
	MT76_TM_ATTR_TX_DUTY_CYCLE,
	MT76_TM_ATTR_TX_IPG,
	MT76_TM_ATTR_TX_TIME,
	MT76_TM_ATTR_DRV_DATA,
	MT76_TM_ATTR_MAC_ADDRS,
};

enum { MT76_TM_STATE_OFF = 0, MT76_TM_STATE_IDLE = 1, MT76_TM_STATE_TX_FRAMES = 2,
       MT76_TM_STATE_RX_FRAMES = 3, MT76_TM_STATE_TX_CONT = 4, MT76_TM_STATE_ON = 5 };

enum { MT76_TM_TX_MODE_CCK = 0, MT76_TM_TX_MODE_OFDM = 1, MT76_TM_TX_MODE_HT = 2,
       MT76_TM_TX_MODE_VHT = 3, MT76_TM_TX_MODE_HE_SU = 4 };

#define NL_BUFSZ 8192

static int nl_fd = -1;
static uint16_t nl_seq = 1;
static int nl80211_id = -1;

static void die(const char *msg)
{
	if (errno)
		fprintf(stderr, "错误: %s: %s\n", msg, strerror(errno));
	else
		fprintf(stderr, "错误: %s\n", msg);
	exit(1);
}

static struct nlattr *nla_put(struct nlmsghdr *nlh, int maxlen, int type,
			      const void *data, int len)
{
	int attr_len = NLA_HDRLEN + len;
	int newlen = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(attr_len);
	struct nlattr *nla;

	if (newlen > maxlen) {
		fprintf(stderr, "错误: netlink 消息超长\n");
		exit(1);
	}
	nla = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
	nla->nla_type = type;
	nla->nla_len = attr_len;
	if (len)
		memcpy((char *)nla + NLA_HDRLEN, data, len);
	memset((char *)nla + attr_len, 0, NLA_ALIGN(attr_len) - attr_len);
	nlh->nlmsg_len = newlen;
	return nla;
}

static void nla_put_u8(struct nlmsghdr *n, int m, int t, uint8_t v)
{ nla_put(n, m, t, &v, sizeof(v)); }
static void nla_put_u32(struct nlmsghdr *n, int m, int t, uint32_t v)
{ nla_put(n, m, t, &v, sizeof(v)); }
static void nla_put_str(struct nlmsghdr *n, int m, int t, const char *s)
{ nla_put(n, m, t, s, strlen(s) + 1); }

static void nl_open(void)
{
	struct sockaddr_nl sa;
	struct timeval tv;

	nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (nl_fd < 0)
		die("socket(AF_NETLINK) 失败");
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		die("bind 失败");

	/* 接收超时：万一内核不回包，也不要让工具永久卡住（路由器上很致命） */
	tv.tv_sec = 3;
	tv.tv_usec = 0;
	setsockopt(nl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int nl_send_recv(struct nlmsghdr *nlh, char *resp, int resp_len)
{
	struct sockaddr_nl sa;
	struct iovec iov;
	struct msghdr msg;
	int ret;
	struct nlmsghdr *h;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;

	iov.iov_base = nlh;
	iov.iov_len = nlh->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (sendmsg(nl_fd, &msg, 0) < 0)
		return -errno;

	iov.iov_base = resp;
	iov.iov_len = resp_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	ret = recvmsg(nl_fd, &msg, 0);
	if (ret < 0)
		return -errno;

	for (h = (struct nlmsghdr *)resp; NLMSG_OK(h, ret); h = NLMSG_NEXT(h, ret)) {
		if (h->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
			return e->error;
		}
	}
	return 0;
}

static int genl_resolve(const char *family)
{
	char buf[NL_BUFSZ];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct nlattr *nla;
	int ret, rem;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = nl_seq++;

	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = CTRL_CMD_GETFAMILY;
	gh->version = 1;
	nla_put_str(nlh, NL_BUFSZ, CTRL_ATTR_FAMILY_NAME, family);

	ret = nl_send_recv(nlh, buf, sizeof(buf));
	if (ret < 0) {
		fprintf(stderr, "解析 netlink family '%s' 失败: %s\n", family, strerror(-ret));
		return -1;
	}

	nlh = (struct nlmsghdr *)buf;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	nla = (struct nlattr *)((char *)gh + GENL_HDRLEN);
	rem = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

	while (rem > 0 && NLA_OK(nla, rem)) {
		if (nla->nla_type == CTRL_ATTR_FAMILY_ID)
			return *(uint16_t *)((char *)nla + NLA_HDRLEN);
		nla = NLA_NEXT(nla, rem);
	}
	return -1;
}

static int testmode_cmd(int ifindex, uint32_t state, int has_tx,
			int chan, int width, int mode, int mcs, int nss,
			int antenna, int power)
{
	char buf[NL_BUFSZ];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct nlattr *td;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = nl80211_id;
	/* 必须带 NLM_F_ACK：nl80211 的 TESTMODE 处理完不回包，
	 * 不请求 ACK 的话下面的 recvmsg 会一直阻塞。 */
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = nl_seq++;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_TESTMODE;
	gh->version = 0;

	if (ifindex > 0)
		nla_put_u32(nlh, NL_BUFSZ, NL80211_ATTR_IFINDEX, ifindex);

	if (chan) {
		int freq = (chan == 14) ? 2484 : (2407 + chan * 5);
		nla_put_u32(nlh, NL_BUFSZ, NL80211_ATTR_WIPHY_FREQ, freq);
	}

	if (width) {
		uint32_t w = NL80211_CHAN_WIDTH_20_NOHT;
		if (width == 40)
			w = NL80211_CHAN_WIDTH_40;
		else if (width == 80)
			w = NL80211_CHAN_WIDTH_80;
		nla_put_u32(nlh, NL_BUFSZ, NL80211_ATTR_CHANNEL_WIDTH, w);
	}

	td = nla_put(nlh, NL_BUFSZ, NL80211_ATTR_TESTDATA, NULL, 0);
	/* ⚠️ STATE 必须按 **u8** 发：
	 *   mt76 的 nla_policy 里  [MT76_TM_ATTR_STATE] = { .type = NLA_U8 }
	 *   但驱动代码里又是        state = nla_get_u32(tb[MT76_TM_ATTR_STATE])
	 * 这两处不一致。按 u32（4 字节）发时，nla_parse_deprecated 会打印
	 *   netlink: 'mt76tm': attribute type 2 has an invalid length.
	 * 然后把这条属性**丢掉**（liberal 模式不算错），于是 tb[STATE]==NULL，
	 * 驱动认为"没改状态"，命令返回 0、什么都不做 —— 2026-11-25 就是被这个坑了一轮，
	 * 工具打印"已进入连续发射"，实际一点波都没发。
	 * 按 u8 发则属性合法；nla_get_u32 读到的 1 字节 + 3 字节补零，值仍然正确。 */
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_STATE, (uint8_t)state);
	if (has_tx) {
		struct nlattr *tp;
		uint8_t pw = (uint8_t)power;

		nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_RATE_MODE, mode);
		nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_RATE_IDX, mcs);
		nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_RATE_NSS, nss);
		nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_ANTENNA, antenna);

		/* TX_POWER 是 NLA_NESTED：驱动用 nla_for_each_nested 逐项读，
		 * 每项必须是长度正好 1 字节的 u8。直接发裸字节会被判 EINVAL。 */
		tp = nla_put(nlh, NL_BUFSZ,
			     MT76_TM_ATTR_TX_POWER | NLA_F_NESTED, NULL, 0);
		nla_put(nlh, NL_BUFSZ, 0, &pw, 1);      /* 天线 0 的功率 */
		if (tp)
			tp->nla_len = (char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len)
				      - (char *)tp;
	}
	td->nla_len = (char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len) - (char *)td;

	return nl_send_recv(nlh, buf, sizeof(buf));
}

/* ---------------- TX_FRAMES 模式：走 MAC 队列灌包 ----------------
 * 为什么要这条路：TX_CONT 依赖固件的 MCU_EXT_CMD(RF_TEST)，
 * 在 MT7981 + 这版 WM 固件上实测会 "Retry message / Message timeout"
 * 然后固件自己重启（dmesg 里能看到 Hardware restart was requested）。
 * TX_FRAMES 不碰那条命令，只写 TM_MAC_TX 寄存器和 MAC 队列，
 * duty=99 + ipg=0 时是背靠背发帧，最接近占满空口。
 * 注意：TX_COUNT 必须给个大数，0 表示"不发"，不是"无线发"。
 */
static int testmode_frames_cmd(int ifindex, int state, int chan, int width,
			       int mode, int mcs, int nss, int antenna, int power,
			       int length, int ipg, int duty, int txtime,
			       uint32_t count)
{
	char buf[NL_BUFSZ];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct nlattr *td, *tp;
	uint8_t pw = (uint8_t)power;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = nl80211_id;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = nl_seq++;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_TESTMODE;
	gh->version = 0;

	if (ifindex > 0)
		nla_put_u32(nlh, NL_BUFSZ, NL80211_ATTR_IFINDEX, ifindex);
	if (chan)
		nla_put_u32(nlh, NL_BUFSZ, NL80211_ATTR_WIPHY_FREQ,
			    (chan == 14) ? 2484 : (2407 + chan * 5));
	if (width) {
		uint32_t w = NL80211_CHAN_WIDTH_20_NOHT;
		if (width == 40)
			w = NL80211_CHAN_WIDTH_40;
		else if (width == 80)
			w = NL80211_CHAN_WIDTH_80;
		nla_put_u32(nlh, NL_BUFSZ, NL80211_ATTR_CHANNEL_WIDTH, w);
	}

	td = nla_put(nlh, NL_BUFSZ, NL80211_ATTR_TESTDATA, NULL, 0);
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_STATE, (uint8_t)state);
	nla_put_u32(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_COUNT, count);
	nla_put_u32(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_LENGTH, (uint32_t)length);
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_RATE_MODE, (uint8_t)mode);
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_RATE_IDX, (uint8_t)mcs);
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_RATE_NSS, (uint8_t)nss);
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_ANTENNA, (uint8_t)antenna);
	nla_put_u8(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_DUTY_CYCLE, (uint8_t)duty);
	nla_put_u32(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_IPG, (uint32_t)ipg);
	nla_put_u32(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_TIME, (uint32_t)txtime);

	tp = nla_put(nlh, NL_BUFSZ, MT76_TM_ATTR_TX_POWER | NLA_F_NESTED, NULL, 0);
	nla_put(nlh, NL_BUFSZ, 0, &pw, 1);
	if (tp)
		tp->nla_len = (char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len) - (char *)tp;
	td->nla_len = (char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len) - (char *)td;

	return nl_send_recv(nlh, buf, sizeof(buf));
}

/* ---------------- 读回内核里的真实状态（验证命令到底生效没有） ----------------
 * 只设不算：2026-11-25 那次工具报"已进入连续发射"，其实属性被内核丢掉、
 * 状态根本没变。所以现在设完必须 dump 一次把 state 读回来比对。
 */
static const char *state_name(uint32_t s)
{
	switch (s) {
	case 0: return "OFF 关闭";
	case 1: return "IDLE";
	case 2: return "TX_FRAMES 灌包";
	case 3: return "RX_FRAMES";
	case 4: return "TX_CONT 连续波";
	case 5: return "ON";
	}
	return "未知";
}

static int nl_dump(int ifindex, char *resp, int resp_len)
{
	char req[512];
	struct nlmsghdr *nlh = (struct nlmsghdr *)req;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa;
	struct iovec iov;
	struct msghdr msg;
	int total = 0;

	memset(req, 0, sizeof(req));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = nl80211_id;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = nl_seq++;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_TESTMODE;
	gh->version = 0;
	if (ifindex > 0)
		nla_put_u32(nlh, sizeof(req), NL80211_ATTR_IFINDEX, ifindex);

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	iov.iov_base = nlh;
	iov.iov_len = nlh->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (sendmsg(nl_fd, &msg, 0) < 0)
		return -errno;

	while (total < resp_len - 512) {
		struct nlmsghdr *h;
		int n, rem, done = 0;

		iov.iov_base = resp + total;
		iov.iov_len = resp_len - total;
		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &sa;
		msg.msg_namelen = sizeof(sa);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		n = recvmsg(nl_fd, &msg, 0);
		if (n <= 0)
			break;
		rem = n;
		for (h = (struct nlmsghdr *)(resp + total); NLMSG_OK(h, rem);
		     h = NLMSG_NEXT(h, rem)) {
			if (h->nlmsg_type == NLMSG_DONE) {
				done = 1;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
				return e->error ? e->error : total + n;
			}
		}
		total += NLMSG_ALIGN(n);
		if (done)
			break;
	}
	return total;
}

static int show_state(int ifindex)
{
	char resp[NL_BUFSZ];
	struct nlmsghdr *h;
	int total, rem, have = 0;

	total = nl_dump(ifindex, resp, sizeof(resp));
	if (total <= 0) {
		printf("读回状态失败: %s (%d)\n",
		       total ? strerror(-total) : "无回包", total);
		return 1;
	}

	rem = total;
	for (h = (struct nlmsghdr *)resp; NLMSG_OK(h, rem); h = NLMSG_NEXT(h, rem)) {
		struct genlmsghdr *gh;
		struct nlattr *nla;
		int arem;

		if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR)
			continue;
		gh = (struct genlmsghdr *)NLMSG_DATA(h);
		nla = (struct nlattr *)((char *)gh + GENL_HDRLEN);
		arem = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
		while (arem > 0 && NLA_OK(nla, arem)) {
			if ((nla->nla_type & NLA_TYPE_MASK) == NL80211_ATTR_TESTDATA) {
				struct nlattr *in = (struct nlattr *)((char *)nla + NLA_HDRLEN);
				int irem = nla->nla_len - NLA_HDRLEN;

				while (irem > 0 && NLA_OK(in, irem)) {
					int it = in->nla_type & NLA_TYPE_MASK;
					const void *p = (char *)in + NLA_HDRLEN;

					if (it == MT76_TM_ATTR_STATE) {
						uint32_t s = *(const uint32_t *)p;
						printf("内核 testmode 状态: %u (%s)\n",
						       s, state_name(s));
						have = 1;
					} else if (it == MT76_TM_ATTR_STATS) {
						struct nlattr *st = (struct nlattr *)p;
						int srem = in->nla_len - NLA_HDRLEN;
						uint32_t pending = 0, queued = 0, done = 0;

						while (srem > 0 && NLA_OK(st, srem)) {
							int sit = st->nla_type & NLA_TYPE_MASK;
							const void *sp = (char *)st + NLA_HDRLEN;

							if (sit == 2)
								pending = *(const uint32_t *)sp;
							else if (sit == 3)
								queued = *(const uint32_t *)sp;
							else if (sit == 4)
								done = *(const uint32_t *)sp;
							st = NLA_NEXT(st, srem);
						}
						printf("  队列: pending=%u queued=%u done=%u\n",
						       pending, queued, done);
					}
					in = NLA_NEXT(in, irem);
				}
			}
			nla = NLA_NEXT(nla, arem);
		}
	}
	if (!have)
		printf("没解析到 testmode 状态（该 phy 可能不支持 dump）\n");
	return have ? 0 : 1;
}

static void usage(void)
{
	fprintf(stderr,
"用法:\n"
"  mt76tm status <ifname>            探测 testmode 是否可用\n"
"  mt76tm check  <ifname>            只读：读回内核里的 testmode 状态\n"
"  mt76tm cont   <ifname> [选项]     连续波（纯载波，依赖固件 RF_TEST）\n"
"  mt76tm frames <ifname> [选项]     灌包（走 MAC 队列，更稳）\n"
"  mt76tm off    <ifname>            停止并恢复正常\n"
"\n"
"通用选项（cont / frames）:\n"
"  -c <信道>   1-13，默认 1\n"
"  -w <带宽>   20|40|80，默认 20\n"
"  -m <模式>   cck|ofdm|ht|vht|he（cont 默认 ofdm，frames 默认 ht）\n"
"  -M <MCS>    0-11\n"
"  -n <流数>   1-2\n"
"  -a <天线>   1|3\n"
"  -p <功率>   0-31，默认 20\n"
"\n"
"frames 专用:\n"
"  -l <字节>   每帧长度，默认 1500\n"
"  -i <us>     帧间隔，默认 0（背靠背）\n"
"  -d <百分比> 占空比 0-99，默认 99\n"
"  -t <us>     每帧占用时长，默认 0\n"
"  -x <帧数>   发多少帧，默认 1e9（够跑很久）\n");
	exit(1);
}

int main(int argc, char **argv)
{
	const char *cmd, *ifname;
	int ifidx, ret;

	if (argc < 3)
		usage();

	nl_open();
	nl80211_id = genl_resolve("nl80211");
	if (nl80211_id < 0)
		return 1;

	cmd = argv[1];
	ifname = argv[2];
	ifidx = if_nametoindex(ifname);
	if (!ifidx) {
		fprintf(stderr, "找不到接口 %s\n", ifname);
		return 1;
	}

	if (!strcmp(cmd, "status")) {
		/* 用 OFF 探测：驱动里 mt76_testmode_set_state(OFF) 在本来就已经
		 * 是 OFF 时会直接 return 0，所以这是**非破坏性**的探针。
		 * 如果 testmode 没编进去，mt76_testmode_cmd 开头就返回 -EOPNOTSUPP。 */
		ret = testmode_cmd(ifidx, MT76_TM_STATE_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
		if (ret == 0) {
			printf("testmode 可用 ✓\n");
			return 0;
		}
		printf("testmode 不可用: %s (%d)\n", strerror(-ret), -ret);
		if (-ret == 95)
			printf("  → 固件没开 CONFIG_NL80211_TESTMODE，需要重编 mt7915e\n");
		return 1;
	}

	if (!strcmp(cmd, "off")) {
		ret = testmode_cmd(ifidx, MT76_TM_STATE_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
		if (ret < 0) {
			fprintf(stderr, "关闭失败: %s\n", strerror(-ret));
			return 1;
		}
		printf("已停止发射。\n");
		return show_state(ifidx);
	}

	/* 只读：把内核里的 testmode 状态读回来（验证设的到底生效没有） */
	if (!strcmp(cmd, "check") || !strcmp(cmd, "state"))
		return show_state(ifidx);

	if (!strcmp(cmd, "frames")) {
		int chan = 1, width = 20, mode = MT76_TM_TX_MODE_HT;
		int mcs = 7, nss = 2, ant = 3, pwr = 20, opt;
		int len = 1500, ipg = 0, duty = 99, txtime = 0;
		uint32_t count = 1000000000u;

		optind = 3;
		while ((opt = getopt(argc, argv, "c:w:m:M:n:a:p:l:i:d:t:x:")) != -1) {
			switch (opt) {
			case 'c': chan = atoi(optarg); break;
			case 'w': width = atoi(optarg); break;
			case 'm':
				if (!strcmp(optarg, "cck")) mode = MT76_TM_TX_MODE_CCK;
				else if (!strcmp(optarg, "ofdm")) mode = MT76_TM_TX_MODE_OFDM;
				else if (!strcmp(optarg, "ht")) mode = MT76_TM_TX_MODE_HT;
				else if (!strcmp(optarg, "vht")) mode = MT76_TM_TX_MODE_VHT;
				else if (!strcmp(optarg, "he")) mode = MT76_TM_TX_MODE_HE_SU;
				else usage();
				break;
			case 'M': mcs = atoi(optarg); break;
			case 'n': nss = atoi(optarg); break;
			case 'a': ant = atoi(optarg); break;
			case 'p': pwr = atoi(optarg); break;
			case 'l': len = atoi(optarg); break;
			case 'i': ipg = atoi(optarg); break;
			case 'd': duty = atoi(optarg); break;
			case 't': txtime = atoi(optarg); break;
			case 'x': count = (uint32_t)strtoul(optarg, NULL, 0); break;
			default: usage();
			}
		}
		if (duty > 99)
			duty = 99;

		printf("开始灌包: ch%d / %dMHz / mode=%d MCS=%d nss=%d ant=0x%x pwr=%d\n"
		       "          长度=%d 字节 duty=%d%% ipg=%d us 帧数=%u\n",
		       chan, width, mode, mcs, nss, ant, pwr, len, duty, ipg, count);

		ret = testmode_frames_cmd(ifidx, MT76_TM_STATE_TX_FRAMES, chan, width,
					  mode, mcs, nss, ant, pwr, len, ipg, duty,
					  txtime, count);
		if (ret < 0) {
			fprintf(stderr, "失败: %s (%d)\n", strerror(-ret), -ret);
			return 1;
		}
		return show_state(ifidx);
	}

	if (!strcmp(cmd, "cont")) {
		int chan = 1, width = 20, mode = MT76_TM_TX_MODE_OFDM;
		int mcs = 0, nss = 1, ant = 1, pwr = 20, opt;

		optind = 3;
		while ((opt = getopt(argc, argv, "c:w:m:M:n:a:p:")) != -1) {
			switch (opt) {
			case 'c': chan = atoi(optarg); break;
			case 'w': width = atoi(optarg); break;
			case 'm':
				if (!strcmp(optarg, "cck")) mode = MT76_TM_TX_MODE_CCK;
				else if (!strcmp(optarg, "ofdm")) mode = MT76_TM_TX_MODE_OFDM;
				else if (!strcmp(optarg, "ht")) mode = MT76_TM_TX_MODE_HT;
				else if (!strcmp(optarg, "vht")) mode = MT76_TM_TX_MODE_VHT;
				else if (!strcmp(optarg, "he")) mode = MT76_TM_TX_MODE_HE_SU;
				else usage();
				break;
			case 'M': mcs = atoi(optarg); break;
			case 'n': nss = atoi(optarg); break;
			case 'a': ant = atoi(optarg); break;
			case 'p': pwr = atoi(optarg); break;
			default: usage();
			}
		}

		printf("开始连续发射: ch%d / %dMHz / mode=%d MCS=%d nss=%d ant=0x%x pwr=%d\n",
		       chan, width, mode, mcs, nss, ant, pwr);

		ret = testmode_cmd(ifidx, MT76_TM_STATE_TX_CONT, 1,
				   chan, width, mode, mcs, nss, ant, pwr);
		if (ret < 0) {
			fprintf(stderr, "失败: %s (%d)\n", strerror(-ret), -ret);
			if (-ret == 95)
				fprintf(stderr,
					"EOPNOTSUPP —— 固件没编 testmode\n"
					"  需要内核开 CONFIG_NL80211_TESTMODE 重新编译 mt7915e\n");
			else if (-ret == 107)
				fprintf(stderr,
					"ENOTCONN —— 射频不在 monitor 模式（驱动强制的）\n"
					"  先用下面这套命令准备好，再发连续波：\n"
					"    wifi down\n"
					"    iw phy phy0 interface add mon0 type monitor\n"
					"    ip link set mon0 up\n"
					"    iw dev mon0 set channel %d\n"
					"    %s cont mon0 -c %d -w %d\n",
					chan, argv[0], chan, width);
			else if (-ret == 22)
				fprintf(stderr,
					"EINVAL —— 参数被驱动拒绝。常见原因：\n"
					"  · 带宽选了 40/80 但信道不支持\n"
					"  · MCS 超出该模式范围（ofdm 只支持 0-8）\n"
					"  · nss 超过天线数\n");
			return 1;
		}
		printf("命令已下发。停止: %s off %s\n", argv[0], ifname);
		return show_state(ifidx);
	}

	usage();
	return 1;
}
