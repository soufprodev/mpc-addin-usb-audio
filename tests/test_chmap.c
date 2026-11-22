/* test_chmap.c: channel map parsing and frame assembly. */
#include "../src/chmap.h"
#include "t.h"

int main(void) {
  mpcua_chsrc m[MPCUA_MAX_CH];
  const unsigned TO_HOST = MPCUA_SRC_OUT | MPCUA_SRC_IN;
  CHECK_EQ(mpcua_chmap_parse("out1,out2,in1,in2", TO_HOST, m, MPCUA_MAX_CH), 4);
  CHECK_EQ(m[0].kind, MPCUA_SRC_OUT); CHECK_EQ(m[0].idx, 0);
  CHECK_EQ(m[3].kind, MPCUA_SRC_IN); CHECK_EQ(m[3].idx, 1);
  CHECK_EQ(mpcua_chmap_parse(" out2 , 0 ,-", TO_HOST, m, MPCUA_MAX_CH), 3);
  CHECK_EQ(m[0].idx, 1); CHECK_EQ(m[1].kind, MPCUA_SRC_NONE); CHECK_EQ(m[2].kind, MPCUA_SRC_NONE);
  CHECK_EQ(mpcua_chmap_parse("host1", TO_HOST, m, MPCUA_MAX_CH), -1);  /* kind not allowed */
  CHECK_EQ(mpcua_chmap_parse("out0", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out33", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out32", TO_HOST, m, MPCUA_MAX_CH), 1);
  /* ranges */
  CHECK_EQ(mpcua_chmap_parse("out1-27", TO_HOST, m, MPCUA_USB_MAX_CH), 27);
  CHECK_EQ(m[0].idx, 0); CHECK_EQ(m[26].idx, 26); CHECK_EQ(m[26].kind, MPCUA_SRC_OUT);
  CHECK_EQ(mpcua_chmap_parse("out1-28", TO_HOST, m, MPCUA_USB_MAX_CH), -1);   /* more than USB carries */
  CHECK_EQ(mpcua_chmap_parse("out3-8, in1-2 ,0", TO_HOST, m, MPCUA_MAX_CH), 9);
  CHECK_EQ(m[0].idx, 2); CHECK_EQ(m[5].idx, 7); CHECK_EQ(m[6].kind, MPCUA_SRC_IN); CHECK_EQ(m[7].idx, 1);
  CHECK_EQ(m[8].kind, MPCUA_SRC_NONE);
  CHECK_EQ(mpcua_chmap_parse("out4-3", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out1-", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out-2", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("host1-27", MPCUA_SRC_HOST, m, MPCUA_MAX_CH), 27);
  CHECK_EQ(mpcua_chmap_parse("out1,", TO_HOST, m, MPCUA_MAX_CH), -1);   /* empty entry */
  CHECK_EQ(mpcua_chmap_parse("outx", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("out1,out1,out1", TO_HOST, m, 2), -1);     /* too many */
  CHECK_EQ(mpcua_chmap_parse("", TO_HOST, m, MPCUA_MAX_CH), -1);

  /* assemble: 4 USB channels from 2 out + 2 in, with out3 missing on a 2-channel device */
  CHECK_EQ(mpcua_chmap_parse("out1,out2,in2,out3", TO_HOST, m, MPCUA_MAX_CH), 4);
  int32_t outf[2] = {10, 20}, inf[2] = {30, 40}, dst[4];
  mpcua_chmap_frame(m, 4, outf, 2, 0, inf, 2, NULL, 0, dst);
  CHECK_EQ(dst[0], 10); CHECK_EQ(dst[1], 20); CHECK_EQ(dst[2], 40); CHECK_EQ(dst[3], 0);
  mpcua_chmap_frame(m, 4, outf, 2, 0, NULL, 0, NULL, 0, dst);              /* no input tap */
  CHECK_EQ(dst[2], 0);

  CHECK_EQ(mpcua_chmap_parse("host2,host1", MPCUA_SRC_HOST, m, MPCUA_MAX_CH), 2);
  int32_t hf[2] = {7, 8};
  mpcua_chmap_frame(m, 2, NULL, 0, 0, NULL, 0, hf, 2, dst);
  CHECK_EQ(dst[0], 8); CHECK_EQ(dst[1], 7);
  /* sends: after the codec channels in the out frame */
  const unsigned ALL = TO_HOST | MPCUA_SRC_SEND;
  CHECK_EQ(mpcua_chmap_parse("out1,out2,send1-25", ALL, m, MPCUA_USB_MAX_CH), 27);
  CHECK_EQ(mpcua_chmap_sends(m, 27), 25u);
  CHECK_EQ(mpcua_chmap_parse("send26", ALL, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("send1", TO_HOST, m, MPCUA_MAX_CH), -1);
  CHECK_EQ(mpcua_chmap_parse("send2,out1", ALL, m, MPCUA_MAX_CH), 2);
  CHECK_EQ(mpcua_chmap_sends(m, 2), 2u);
  int32_t of[4] = {1, 2, 30, 40};
  mpcua_chmap_frame(m, 2, of, 2, 2, NULL, 0, NULL, 0, dst);
  CHECK_EQ(dst[0], 40); CHECK_EQ(dst[1], 1);
  mpcua_chmap_frame(m, 2, of, 2, 1, NULL, 0, NULL, 0, dst);   /* send2 not live: silence */
  CHECK_EQ(dst[0], 0);
  T_DONE("chmap");
}
