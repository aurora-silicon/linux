#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <linux/types.h>

struct device;
struct platform_device;
struct dcp_sound_cookie;

int dcp_audiosrv_prepare(struct device *dev, struct dcp_sound_cookie *cookie);
int dcp_audiosrv_startlink(struct device *dev, struct dcp_sound_cookie *cookie);
int dcp_audiosrv_stoplink(struct device *dev);
int dcp_audiosrv_unprepare(struct device *dev);
int dcp_audiosrv_get_elements(struct device *dev, void *elements, size_t maxsize);
int dcp_audiosrv_get_product_attrs(struct device *dev, void *attrs, size_t maxsize);
int dcp_audio_sink_eld(struct device *dev, u8 *eld, size_t size);

void dcpaud_connect(struct platform_device *pdev, bool connected);
void dcpaud_disconnect(struct platform_device *pdev);

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_AUDIO)
/* Native link methods for 14.7 and desktop 26.x external roles. */
enum dcp_audio_link {
	DCP_AUDIO_LINK_PREPARE,
	DCP_AUDIO_LINK_START,
	DCP_AUDIO_LINK_STOP,
	DCP_AUDIO_LINK_COMPLETE,
	/* The started and stopping notifications around START and STOP */
	DCP_AUDIO_LINK_DID_START,
	DCP_AUDIO_LINK_WILL_STOP,
	/* The link's state, asked as a stream starts and stops (firmware 27) */
	DCP_AUDIO_LINK_QUERY,
};

/*
 * Firmware 27's link data: the stream's format, which dcp_audiosrv_link()
 * sends at body+48 in place of the element's ElementData.
 */
struct dcp_audio_link_format {
	__le32 format;
	__le32 channels;
	__le32 sample_size;
	__le32 rate;
} __packed;

/* Whether the link data is the stream's format (firmware 27's external role) */
bool dcp_audiosrv_link_by_format(struct device *dev);

bool dcp_audiosrv_native_link(struct device *dev);
int dcp_audiosrv_open(struct device *dev);
void dcp_audiosrv_close(struct device *dev);
int dcp_audiosrv_link(struct device *dev, enum dcp_audio_link op,
		      const struct dcp_sound_cookie *cookie);
/* @gen: only a link prepared on that instance, still the open one (NULL: any) */
void dcpaud_release_link(struct platform_device *pdev, const u64 *gen);
/* Holds the connection's default link before the display's mode is set */
void dcpaud_before_modeset(struct platform_device *pdev);
/* Whether the link prepared last is on instance @gen, still the open one */
bool dcp_audiosrv_link_on(struct device *dev, u64 gen);
/* The prepared link still belongs to the client of the published instance. */
bool dcp_audiosrv_link_live(struct device *dev);
#endif

#endif /* __AUDIO_H__ */
