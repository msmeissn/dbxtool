/*
 * Copyright 2014 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esl.h"
#include "iter.h"
#include "util.h"
#include "x509.h"

struct esd_iter {
	esl_iter *iter;
	int line;

	EFI_SIGNATURE_DATA *esd;
	size_t len;

	size_t nmemb;
	unsigned int i;
};

int
esd_iter_new(esd_iter **iter, uint8_t *buf, size_t len)
{
	if (len < sizeof (EFI_SIGNATURE_LIST) + sizeof (EFI_SIGNATURE_DATA)) {
		errno = EINVAL;
		return -1;
	}

	*iter = calloc(1, sizeof (esd_iter));
	if (!*iter)
		err(1, NULL);

	int rc;

	rc = esl_iter_new(&(*iter)->iter, buf, len);
	if (rc < 0) {
		save_errno(free(*iter));
		return -1;
	}

	(*iter)->i = -1;

	return 0;
}

int
esd_iter_end(esd_iter *iter)
{
	if (!iter) {
		errno = EINVAL;
		return -1;
	}
	if (iter->iter)
		esl_iter_end(iter->iter);
	free(iter);
	return 0;
}

int
esd_iter_next(esd_iter *iter, efi_guid_t *type, efi_guid_t *owner,
		uint8_t **data, size_t *len)
{
	int rc;
	size_t ss;

	if (!iter)
		return -EINVAL;

	if (iter->iter == NULL)
		return -EINVAL;

	iter->line += 1;

	iter->i += 1;
	if (iter->i == iter->nmemb) {
		vprintf("Getting next EFI_SIGNATURE_DATA\n");
		iter->i = 0;
		rc = esl_iter_next(iter->iter, type, &iter->esd, &iter->len);
		if (rc < 1)
			return rc;

		if (!efi_guid_cmp(type, &efi_guid_x509_cert)) {
			int32_t asn1size;

			asn1size = GetASN1SequenceSize(iter->esd->SignatureData,
				iter->len - sizeof (iter->esd->SignatureOwner));

			if (asn1size < 0) {
				warnx("iterator data claims to be an X.509 "
				      "Cert but is not valid ASN.1 DER");
			} else if ((uint32_t)asn1size != iter->len -
					sizeof (iter->esd->SignatureOwner)) {
				warnx("X.509 Cert ASN.1 size does not match "
				      "SignatureList Size (%d vs %ld)",
				      asn1size, iter->len -
					sizeof (iter->esd->SignatureOwner));
			}
		}

		size_t sls, slh;
		rc = esl_list_size(iter->iter, &sls);
		if (rc < 0)
			return rc;

		rc = esl_header_size(iter->iter, &slh);
		if (rc < 0)
			return rc;

		rc = esl_sig_size(iter->iter, &ss);
		if (rc < 0)
			return rc;

		/* if we'd have leftover data, then this ESD is garbage. */
		if ((sls - sizeof (EFI_SIGNATURE_LIST) - slh) % ss != 0)
			return -EINVAL;

		iter->nmemb = (sls - sizeof (EFI_SIGNATURE_LIST) - slh) / ss;
	} else {
		vprintf("Getting next esd element\n");
		rc = esl_sig_size(iter->iter, &ss);
		if (rc < 0)
			return rc;

		iter->esd = (EFI_SIGNATURE_DATA *)((intptr_t)iter->esd + ss);
	}

	rc = esl_get_type(iter->iter, type);
	if (rc < 0)
		return rc;
	*owner = iter->esd->SignatureOwner;
	*data = iter->esd->SignatureData;
	*len = ss - sizeof (iter->esd->SignatureOwner);
	return 1;
}

int
esd_iter_get_line(esd_iter *iter)
{
	if (!iter) {
		errno = EINVAL;
		return -1;
	}

	return iter->line;
}

struct esl_iter {
	uint8_t *buf;
	size_t len;

	off_t offset;

	EFI_SIGNATURE_LIST *esl;
};

int
esl_iter_new(esl_iter **iter, uint8_t *buf, size_t len)
{
	if (len < sizeof (EFI_SIGNATURE_LIST) + sizeof (EFI_SIGNATURE_DATA)) {
		errno = EINVAL;
		return -1;
	}

	*iter = calloc(1, sizeof (esl_iter));
	if (!*iter)
		err(1, NULL);

	(*iter)->buf = buf;
	(*iter)->len = len;

	return 0;
}

int
esl_iter_end(esl_iter *iter)
{
	if (!iter) {
		errno = EINVAL;
		return -1;
	}
	free(iter);
	return 0;
}

int
esl_iter_next(esl_iter *iter, efi_guid_t *type,
		EFI_SIGNATURE_DATA **data, size_t *len)
{
	if (!iter)
		return -EINVAL;
	if (iter->offset < 0)
		return -EINVAL;
	if ((uint32_t)iter->offset >= iter->len)
		return -EINVAL;

	if (!iter->esl) {
		vprintf("Getting next ESL buffer\n");
		iter->esl = (EFI_SIGNATURE_LIST *)iter->buf;
	} else {
		vprintf("Getting next EFI_SIGNATURE_LIST\n");
		efi_guid_t type;
		esl_get_type(iter, &type);
		if (iter->len - iter->offset > iter->esl->SignatureListSize) {
			warnx("EFI Signature List is malformed");
			errx(1, "list has %zd bytes left, element is %"PRIu32" bytes",
			     iter->len - iter->offset,
			     iter->esl->SignatureListSize);
		}
		if (!efi_guid_cmp(&type, &efi_guid_x509_cert)) {
			int32_t asn1size;

			asn1size = GetASN1SequenceSize(
				((uint8_t *)*data) + sizeof (efi_guid_t),
				*len - sizeof (efi_guid_t));
			if (asn1size < 0) {
				warnx("iterator data claims to be an X.509 "
				      "Cert but is not valid ASN.1 DER");
			} else if ((uint32_t)asn1size != iter->esl->SignatureSize
							 - sizeof (efi_guid_t)) {
				warnx("X.509 Cert ASN.1 size does not match "
				      "SignatureList Size (%d vs %ld)",
				      asn1size, iter->esl->SignatureSize -
						sizeof (efi_guid_t));
			}

		}

		iter->offset += iter->esl->SignatureListSize;
		if ((uint32_t)iter->offset >= iter->len)
			return 0;
		iter->esl = (EFI_SIGNATURE_LIST *)((intptr_t)iter->buf
						+ iter->offset);
	}

	EFI_SIGNATURE_LIST esl;
	memset(&esl, '\0', sizeof (esl));
	/* if somehow we've gotten a buffer that's bigger than our
	 * real list, this will be zeros, so we've hit the end. */
	if (!memcmp(&esl, iter->esl, sizeof (esl)))
		return 0;

	/* if this list size is too big for our data, then it's malformed
	 * data and we're done. */
	if (iter->esl->SignatureListSize > iter->len - iter->offset)
		return -EINVAL;

	*type = iter->esl->SignatureType;
	*data = (EFI_SIGNATURE_DATA *)((intptr_t)iter->esl
			+ sizeof (EFI_SIGNATURE_LIST)
			+ iter->esl->SignatureHeaderSize);
	*len = iter->esl->SignatureListSize - sizeof (EFI_SIGNATURE_LIST);

	return 1;
}

int
esl_list_size(esl_iter *iter, size_t *sls)
{
	if (!iter || !iter->esl) {
		errno = EINVAL;
		return -1;
	}
	/* this has to be at least as large as its header to be valid */
	if (iter->esl->SignatureListSize < sizeof (EFI_SIGNATURE_LIST)) {
		errno = EINVAL;
		return -1;
	}

	*sls = iter->esl->SignatureListSize;
	return 0;
}

int
esl_header_size(esl_iter *iter, size_t *slh)
{
	if (!iter || !iter->esl) {
		errno = EINVAL;
		return -1;
	}

	*slh = iter->esl->SignatureHeaderSize;
	return 0;
}

int
esl_sig_size(esl_iter *iter, size_t *ss)
{
	if (!iter || !iter->esl) {
		errno = EINVAL;
		return -1;
	}
	/* If signature size isn't positive, there's invalid data. */
	if (iter->esl->SignatureSize < 1) {
		errno = EINVAL;
		return -1;
	}

	*ss = iter->esl->SignatureSize;
	return 0;
}

int
esl_get_type(esl_iter *iter, efi_guid_t *type)
{
	if (!iter || !iter->esl) {
		errno = EINVAL;
		return -1;
	}

	memcpy(type, &iter->esl->SignatureType, sizeof (*type));
	return 0;
}
