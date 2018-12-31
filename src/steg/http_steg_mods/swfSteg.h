/* Copyright 2011, 2012 SRI International
* See LICENSE for other credits and copying information
*/

#ifndef _SWFSTEG_H
#define _SWFSTEG_H

//struct payloads;

#define SWF_SAVE_HEADER_LEN 1500
#define SWF_SAVE_FOOTER_LEN 1500

class SWFSteg : public FileStegMod
{

public:
  /** 
   * overriding the parent function to return false because SWFSteg changes 
   * the cover length after injecting data in them
   * 
   * @return false indicating that swfSteg can change the length of the cover
   */
  virtual bool cover_lenght_preserving() { return false; };


 /**
compute the capcaity of the cover by getting a pointer to the
beginig of the body in the response

@param cover_body pointer to the begiing of the body
@param body_length the total length of message body
*/
    virtual ssize_t headless_capacity(char *cover_body, int body_length);
    static unsigned int static_headless_capacity(char *cover_body, int body_length);

    /**
returns the capacity of the data you can store in jpeg response
given the jpeg file content in

@param buffer: the buffer containing the payload
@param len: the buffer's length

@return the capacity that the buffer can cover or < 0 in case of error
*/
virtual ssize_t capacity(const uint8_t *buffer, size_t len);
static unsigned int static_capacity(char *buffer, int len);

    /**
constructor just to call parent constructor
*/
    SWFSteg(PayloadServer* payload_provider, double noise2signal = 0);

    virtual int encode(uint8_t* data, size_t data_len, uint8_t* cover_payload, size_t cover_len);
    
     virtual ssize_t decode(const uint8_t* cover_payload, size_t cover_len, uint8_t* data);


};


#endif
