#include "extensions/filters/http/aws/lambda_filter.h"
#include "extensions/filters/http/aws/lambda_filter_config_factory.h"

#include "test/extensions/filters/http/aws/mocks.h"
#include "test/mocks/common.h"
#include "test/mocks/server/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "fmt/format.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AtLeast;
using testing::Invoke;
using testing::Return;
using testing::ReturnPointee;
using testing::ReturnRef;
using testing::SaveArg;
using testing::WithArg;
using testing::_;

namespace Envoy {
namespace Http {

using Server::Configuration::LambdaFilterConfigFactory;

// Nothing here as we mock the function retriever
class NothingMetadataAccessor : public MetadataAccessor {
public:
  virtual absl::optional<const std::string *> getFunctionName() const {
    return {};
  }
  virtual absl::optional<const ProtobufWkt::Struct *> getFunctionSpec() const {
    return {};
  }
  virtual absl::optional<const ProtobufWkt::Struct *>
  getClusterMetadata() const {
    return {};
  }
  virtual absl::optional<const ProtobufWkt::Struct *> getRouteMetadata() const {
    return {};
  }

  virtual ~NothingMetadataAccessor() {}
};

class LambdaFilterTest : public testing::Test {
public:
  LambdaFilterTest() {}

protected:
  void SetUp() override {
    function_retriever_ = std::make_shared<NiceMock<MockFunctionRetriever>>();
    filter_ = std::make_unique<LambdaFilter>(function_retriever_);
    filter_->setDecoderFilterCallbacks(filter_callbacks_);
  }

  NiceMock<MockStreamDecoderFilterCallbacks> filter_callbacks_;
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;

  std::unique_ptr<LambdaFilter> filter_;
  std::shared_ptr<NiceMock<MockFunctionRetriever>> function_retriever_;
};

// see:
// https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
TEST_F(LambdaFilterTest, SingsOnHeadersEndStream) {
  // const FunctionalFilterBase& filter = static_cast<const
  // FunctionalFilterBase&>(*filter_);
  EXPECT_CALL(*function_retriever_, getFunction(_)).Times(1);

  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};
  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));

  // Check aws headers.
  EXPECT_TRUE(headers.has("Authorization"));
}

TEST_F(LambdaFilterTest, SingsOnDataEndStream) {

  EXPECT_CALL(*function_retriever_, getFunction(_)).Times(1);

  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};
  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(headers, false));
  EXPECT_FALSE(headers.has("Authorization"));
  Buffer::OwnedImpl data("data");

  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data, true));

  EXPECT_TRUE(headers.has("Authorization"));
}

// see: https://docs.aws.amazon.com/lambda/latest/dg/API_Invoke.html
TEST_F(LambdaFilterTest, CorrectFuncCalled) {
  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};
  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));

  EXPECT_EQ("/2015-03-31/functions/" + function_retriever_->name_ +
                "/invocations?Qualifier=" + function_retriever_->qualifier_,
            headers.get_(":path"));
}

// see: https://docs.aws.amazon.com/lambda/latest/dg/API_Invoke.html
TEST_F(LambdaFilterTest, FuncWithoutQualifierCalled) {

  EXPECT_CALL(*function_retriever_, getFunction(_))
      .WillRepeatedly(Return(Function{&function_retriever_->name_,
                                      {},
                                      function_retriever_->async_,
                                      &function_retriever_->host_,
                                      &function_retriever_->region_,
                                      &function_retriever_->access_key_,
                                      &function_retriever_->secret_key_}));

  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};

  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));

  EXPECT_EQ("/2015-03-31/functions/" + function_retriever_->name_ +
                "/invocations",
            headers.get_(":path"));
}

TEST_F(LambdaFilterTest, FuncWithEmptyQualifierCalled) {
  function_retriever_->qualifier_ = "";
  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};

  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));

  EXPECT_EQ("/2015-03-31/functions/" + function_retriever_->name_ +
                "/invocations",
            headers.get_(":path"));
}

TEST_F(LambdaFilterTest, AsyncCalled) {
  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};

  function_retriever_->async_ = true;
  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));
  EXPECT_EQ("Event", headers.get_("x-amz-invocation-type"));
}

TEST_F(LambdaFilterTest, SyncCalled) {
  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};

  function_retriever_->async_ = false;
  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));
  EXPECT_EQ("RequestResponse", headers.get_("x-amz-invocation-type"));
}

TEST_F(LambdaFilterTest, SignOnTrailedEndStream) {
  EXPECT_CALL(*function_retriever_, getFunction(_)).Times(1);
  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};

  ASSERT_EQ(true, filter_->retrieveFunction(NothingMetadataAccessor()));
  EXPECT_EQ(FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(headers, false));
  Buffer::OwnedImpl data("data");

  EXPECT_EQ(FilterDataStatus::StopIterationAndBuffer,
            filter_->decodeData(data, false));
  EXPECT_FALSE(headers.has("Authorization"));

  TestHeaderMapImpl trailers;
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(trailers));

  EXPECT_TRUE(headers.has("Authorization"));
}

TEST_F(LambdaFilterTest, InvalidFunction) {
  // invalid function
  EXPECT_CALL(*function_retriever_, getFunction(_))
      .WillRepeatedly(Return(absl::optional<Function>()));

  TestHeaderMapImpl headers{{":method", "GET"},
                            {":authority", "www.solo.io"},
                            {":path", "/getsomething"}};

  EXPECT_EQ(false, filter_->retrieveFunction(NothingMetadataAccessor()));
}

} // namespace Http
} // namespace Envoy
