// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.forms.CertificateParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;

import io.ebean.*;
import io.ebean.annotation.*;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.annotation.JsonFormat;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;
import play.data.validation.Constraints;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Enumerated;
import javax.persistence.EnumType;
import javax.persistence.Id;

import java.io.IOException;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.UUID;
import java.util.Set;
import java.util.stream.Collectors;

@Entity
public class CertificateInfo extends Model {

  public enum Type {
    @EnumValue("SelfSigned")
    SelfSigned,

    @EnumValue("CustomCertHostPath")
    CustomCertHostPath
  }

  @Constraints.Required
  @Id
  @Column(nullable = false, unique = true)
  public UUID uuid;

  @Constraints.Required
  @Column(nullable = false)
  public UUID customerUUID;

  @Column(unique = true)
  public String label;

  @Constraints.Required
  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  public Date startDate;

  @Constraints.Required
  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  public Date expiryDate;

  @Column(nullable = true)
  public String privateKey;

  @Constraints.Required
  @Column(nullable = false)
  public String certificate;

  @Constraints.Required
  @Column(nullable = false)
  @Enumerated(EnumType.STRING)
  public CertificateInfo.Type certType;

  // For mTLS, each certificate will also be requiring a corresponding certificate
  // and key file for connections to the DB nodes.
  @Column(nullable = true)
  public String platformCert;
  public void setPlatformCert(String certPath) {
    this.platformCert = certPath;
    this.save();
  }
  @Column(nullable = true)
  public String platformKey;
  public void setPlatformKey(String keyPath) {
    this.platformKey = keyPath;
    this.save();
  }

  @Column(nullable = true)
  public String checksum;
  public void setChecksum() throws IOException, NoSuchAlgorithmException {
    if (this.certificate != null) {
      this.checksum = Util.getFileChecksum(this.certificate);
      this.save();
    }
  }

  @Column(columnDefinition = "TEXT", nullable = true)
  @DbJson
  public JsonNode customCertInfo;
  public CertificateParams.CustomCertInfo getCustomCertInfo() {
    if (this.customCertInfo != null) {
        return Json.fromJson(this.customCertInfo, CertificateParams.CustomCertInfo.class);
    }
    return null;
  }
  public void setCustomCertInfo(CertificateParams.CustomCertInfo certInfo) {
    this.customCertInfo = Json.toJson(certInfo);
    this.save();
  }

  public static final Logger LOG = LoggerFactory.getLogger(CertificateInfo.class);

  // Create function for self-signed certs.
  public static CertificateInfo create(
      UUID uuid, UUID customerUUID, String label, Date startDate, Date expiryDate,
      String certificate, String privateKey, String platformCert, String platformKey)
      throws IOException, NoSuchAlgorithmException {
    CertificateInfo cert = create(uuid, customerUUID, label, startDate, expiryDate, certificate,
        platformCert, platformKey);
    cert.certType = CertificateInfo.Type.SelfSigned;
    cert.privateKey = privateKey;
    cert.save();
    return cert;
  }

  // Create function for custom certs.
  public static CertificateInfo create(
      UUID uuid, UUID customerUUID, String label, Date startDate, Date expiryDate,
      String certificate, CertificateParams.CustomCertInfo customCertInfo,
      String platformCert, String platformKey)
      throws IOException, NoSuchAlgorithmException {
    CertificateInfo cert = create(uuid, customerUUID, label, startDate, expiryDate, certificate,
        platformCert, platformKey);
    cert.certType = Type.CustomCertHostPath;
    cert.customCertInfo = Json.toJson(customCertInfo);
    cert.save();
    return cert;
  }

  // Create function for setting the common values.
  public static CertificateInfo create(
      UUID uuid, UUID customerUUID, String label, Date startDate, Date expiryDate,
      String certificate, String platformCert, String platformKey)
      throws IOException, NoSuchAlgorithmException {
    CertificateInfo cert = new CertificateInfo();
    cert.uuid = uuid;
    cert.customerUUID = customerUUID;
    cert.label = label;
    cert.startDate = startDate;
    cert.expiryDate = expiryDate;
    cert.certificate = certificate;
    cert.platformCert = platformCert;
    cert.platformKey = platformKey;
    cert.checksum = Util.getFileChecksum(certificate);
    return cert;
  }

  private static final Finder<UUID, CertificateInfo> find =
    new Finder<UUID, CertificateInfo>(CertificateInfo.class) {};

  public static CertificateInfo get(UUID certUUID) {
    return find.byId(certUUID);
  }

  public static CertificateInfo get(String label) {
    return find.query().where().eq("label", label).findOne();
  }

  public static List<CertificateInfo> getAllNoChecksum() {
    return find.query().where().isNull("checksum").findList();
  }

  public static List<CertificateInfo> getAll(UUID customerUUID) {
    return find.query().where().eq("customer_uuid", customerUUID).findList();
  }

  public static boolean isCertificateValid(UUID certUUID) {
    if (certUUID == null) {
      return true;
    }
    CertificateInfo certificate = CertificateInfo.get(certUUID);
    if (certificate == null) {
      return false;
    }
    if (certificate.certType == CertificateInfo.Type.CustomCertHostPath &&
        certificate.customCertInfo == null) {
      return false;
    }
    return true;
  }

  // Returns if there is an in use reference to the object.
  public boolean getInUse() {
    return Universe.existsCertificate(this.uuid, this.customerUUID);
  }

  public ArrayNode getUniverseDetails() {
    Set<Universe> universes = Universe.universeDetailsIfCertsExists(this.uuid, this.customerUUID);
    return Util.getUniverseDetails(universes);
  }
}
