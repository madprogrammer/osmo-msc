#ifndef _GPRS_GMM_H
#define _GPRS_GMM_H

int gsm48_tx_gsm_act_pdp_rej(struct sgsn_mm_ctx *mm, uint8_t tid,
			     uint8_t cause, uint8_t pco_len, uint8_t *pco_v);
int gsm48_tx_gsm_act_pdp_acc(struct sgsn_pdp_ctx *pdp);
int gsm48_tx_gsm_deact_pdp_acc(struct sgsn_pdp_ctx *pdp);

int gsm0408_gprs_rcvmsg(struct msgb *msg);

#endif /* _GPRS_GMM_H */
