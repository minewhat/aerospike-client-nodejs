/*******************************************************************************
 * Copyright 2013 Aerospike Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy 
 * of this software and associated documentation files (the "Software"), to 
 * deal in the Software without restriction, including without limitation the 
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or 
 * sell copies of the Software, and to permit persons to whom the Software is 
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in 
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 ******************************************************************************/

extern "C" {
        #include <aerospike/aerospike.h>
        #include <aerospike/aerospike_key.h>
        #include <aerospike/as_config.h>
        #include <aerospike/as_key.h>
        #include <aerospike/as_record.h>
	#include <aerospike/aerospike_batch.h>
}

#include <node.h>
#include <cstdlib>
#include <unistd.h>

#include "../client.h"
#include "../util/async.h"
#include "../util/conversions.h"
#include "../util/log.h"

using namespace v8;

/*******************************************************************************
 *      TYPES
 ******************************************************************************/

/**
 *      AsyncData — Data to be used in async calls.
 */
typedef struct AsyncData {
	aerospike * as;
	int param_err;
	as_error err;
	as_batch batch;
	as_batch_read  *results;
	uint32_t n;
	Persistent<Function> callback;
} AsyncData;



/*******************************************************************************
 *      FUNCTIONS
 ******************************************************************************/

bool batch_callback(const as_batch_read * results, uint32_t n, void * udata)
{
	printf("V8 callback being called \n");
	// Fetch the AsyncData structure
	AsyncData *     data    = reinterpret_cast<AsyncData *>(udata);
	//copy the batch result to the shared data structure AsyncData,
	//so that response can send it back to nodejs layer
	//as_batch_read  *batch_result = &data->results;
	if( results != NULL) {
		data->n = n;
		data->results = (as_batch_read *)calloc(n, sizeof(as_batch_read));
		for ( uint32_t i = 0; i < n; i++) {
			data->results[i].result = results[i].result; 
			as_record * rec = NULL;
			if (results[i].result == AEROSPIKE_OK) {	
				rec = &data->results[i].record;
				as_record_init(rec, results[i].record.bins.size);
				key_copy_constructor(results[i].key, (as_key**) &data->results[i].key); 
				record_copy_constructor(&results[i].record, &rec);
				
			} 
		}
		return true;
	}
	return false;
}
/**
 *      prepare() — Function to prepare AsyncData, for use in `execute()` and `respond()`.
 *  
 *      This should only keep references to V8 or V8 structures for use in 
 *      `respond()`, because it is unsafe for use in `execute()`.
 */
static void * prepare(const Arguments& args)
{
        // The current scope of the function
        HandleScope scope;

        AerospikeClient * client = ObjectWrap::Unwrap<AerospikeClient>(args.This());

        // Build the async data
        AsyncData *     data = new AsyncData;
        data->as = &client->as;
		data->param_err = 0;

        // Local variables
        as_batch * batch = &data->batch;

        if ( args[0]->IsArray() ) {
			Local<Array> keys = Local<Array>::Cast(args[0]);
            batch_from_jsarray(batch, keys);
        }
		else {
			data->param_err = 1;
		}

        data->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));	

        return data;
}
/**
 *      execute() — Function to execute inside the worker-thread.
 *  
 *      It is not safe to access V8 or V8 data structures here, so everything
 *      we need for input and output should be in the AsyncData structure.
 */
static void execute(uv_work_t * req)
{
    // Fetch the AsyncData structure
    AsyncData * data = reinterpret_cast<AsyncData *>(req->data);

    // Data to be used.
    aerospike *     as      = data->as;
    as_error  *     err     = &data->err;
	as_batch  * 	batch   = &data->batch;

 	// Invoke the blocking call.
    // The error is handled in the calling JS code.
    if( data->param_err == 0) {
	    aerospike_batch_get(as, err, NULL, batch, batch_callback, (void*) req->data);
	}
	
}


/**
 *	respond() — Function to be called after `execute()`. Used to send response
 *  to the callback.
 *  
 *	This function will be run inside the main event loop so it is safe to use 
 *	V8 again. This is where you will convert the results into V8 types, and 
 *	call the callback function with those results.
 */
static void respond(uv_work_t * req, int status)
{
	// Scope for the callback operation.
	HandleScope scope;

	// Fetch the AsyncData structure
	AsyncData *	data	= reinterpret_cast<AsyncData *>(req->data);
	as_error *	err		= &data->err;
	uint32_t num_rec = data->n;
	as_batch_read* batch_results = data->results;


	// Build the arguments array for the callback
	int num_args = 3;
	Handle<Value> argv[num_args] ;
	Handle<Array> arr;
	if(data->param_err == 1) {
		err->code =  AEROSPIKE_ERR_PARAM;
		err->message[0] = '\0';
        err->func = NULL;
        err->line = NULL;
        err->file = NULL;
		argv[0] = error_to_jsobject(err);
		argv[1] = Null();
		argv[2] = Null();
	}
	else {
		arr=Array::New(num_rec);		
		for ( uint32_t i = 0; i< num_rec; i++) {
			Handle<Object> obj = Object::New();
			obj->Set(String::NewSymbol("status"), Integer::New(batch_results[i].result));
			if(batch_results[i].result == AEROSPIKE_OK) {		
				obj->Set(String::NewSymbol("Record"),record_to_jsobject( &batch_results[i].record, batch_results[i].key));
				as_key_destroy((as_key*) batch_results[i].key);
				as_record_destroy(&batch_results[i].record);
			}
			arr->Set(i,obj);
		}
		argv[0] = error_to_jsobject(err);
		argv[1] = Integer::New(num_rec);
		argv[2] = arr;	
	}

	// Surround the callback in a try/catch for safety`
	TryCatch try_catch;

	// Execute the callback.
	data->callback->Call(Context::GetCurrent()->Global(), num_args, argv);

	// Process the exception, if any
	if ( try_catch.HasCaught() ) {
		node::FatalException(try_catch);
	}
	
	// Dispose the Persistent handle so the callback
	// function can be garbage-collected
	data->callback.Dispose();

	// clean up any memory we allocated
	
	delete data;
	delete req;
}

/*******************************************************************************
 *  OPERATION
 ******************************************************************************/

/**
 *      The 'batch_get()' Operation
 */
Handle<Value> AerospikeClient::Batch_Get(const Arguments& args)
{
        return async_invoke(args, prepare, execute, respond);
}

